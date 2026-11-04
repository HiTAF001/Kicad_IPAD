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
#include "gerber.h"
#include "schematic.h"
#include "sch_pick.h"
#include "document.h"
#include "edit.h"
#include "sch_document.h"
#include "sch_edit.h"
#include "geom.h"
#include "route.h"
#include "sexpr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <set>
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
    std::vector<std::string> netDisplay; // net names without KiCad escapes (filled on demand)
    std::shared_ptr<const kp::Schematic> sch; // schematic boards: the whole hierarchy
    int                                  sheet = 0;
    std::string                          schScratch;
    kp::SchPickCache                     schPick;
    struct SchSymbolEntry
    {
        int         sheet, index;
        std::string ref, value;
    };
    std::vector<SchSymbolEntry>          schSymbols; // whole hierarchy, filled on demand
    std::vector<std::string>             schSheetSymbolRefs, schSheetSymbolValues; // current sheet
    std::shared_ptr<const kp::SchDocState> schDoc; // boards built from a kp_sch_doc
    std::vector<std::pair<std::string, std::string>> schFields; // fields of one symbol (on demand)
    int                                  schFieldsSymbol = -1;
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

struct kp_sch_doc
{
    kp::SchDocument                        doc;
    std::shared_ptr<const kp::SchDocState> cache; // schematic of the latest state built
    std::string                            textScratch;
    std::string                            labelScratch;
};

static void copyError( char* aDst, size_t aSize, const std::string& aMsg )
{
    if( !aDst || aSize == 0 )
        return;

    size_t n = std::min( aSize - 1, aMsg.size() );
    std::memcpy( aDst, aMsg.data(), n );
    aDst[n] = 0;
}


/// Load a board with aLoader (which fills kp::Board or throws), then build the mesh, the
/// connectivity (KiCad boards only) and the statistics.
extern "C" int32_t kp_sch_net_sheets( const kp_board* board, int32_t net, int32_t* out, int32_t max_out )
{
    if( !board || !board->sch || net <= 0 || net > (int32_t) board->sch->nets.size() )
        return 0;

    std::vector<int> sheets = kp::SchNetSheets( *board->sch, net - 1 );

    for( int32_t i = 0; out && i < (int32_t) sheets.size() && i < max_out; ++i )
        out[i] = sheets[(size_t) i];

    return (int32_t) sheets.size();
}


static void schSymbolList( const kp_board* board )
{
    kp_board* b = const_cast<kp_board*>( board );

    if( !b->schSymbols.empty() || !board->sch )
        return;

    const kp::Schematic& sch = *board->sch;

    for( size_t si = 0; si < sch.sheets.size(); ++si )
    {
        const kp::SheetNode& node = sch.sheets[si];

        if( node.screen < 0 )
            continue;

        const kp::SchScreen& sc = sch.screens[(size_t) node.screen];

        for( size_t i = 0; i < sc.symbols.size(); ++i )
        {
            const kp::SchSymbol& sym = sc.symbols[i];
            std::string          value;

            for( const kp::SchField& f : sym.fields )
            {
                if( f.name == "Value" )
                    value = kp::DisplayNetName( sch.FieldValue( node, sym, f ) );
            }

            b->schSymbols.push_back( { (int) si, (int) i, kp::SchematicShownReference( sch, node, sym ), value } );
        }
    }
}


extern "C" int32_t kp_sch_symbol_count( const kp_board* board )
{
    if( !board || !board->sch )
        return 0;

    schSymbolList( board );
    return (int32_t) board->schSymbols.size();
}


extern "C" int32_t kp_sch_symbol_entry( const kp_board* board, int32_t i, int32_t* sheet, int32_t* index,
                                        const char** reference, const char** value )
{
    if( !board || !board->sch )
        return 0;

    schSymbolList( board );

    if( i < 0 || i >= (int32_t) board->schSymbols.size() )
        return 0;

    const auto& e = board->schSymbols[(size_t) i];

    if( sheet )
        *sheet = e.sheet;
    if( index )
        *index = e.index;
    if( reference )
        *reference = e.ref.c_str();
    if( value )
        *value = e.value.c_str();

    return 1;
}


extern "C" const char* kp_sch_netlist( kp_board* board )
{
    if( !board || !board->sch )
        return "";

    board->schScratch = kp::SchematicNetlistText( *board->sch );
    return board->schScratch.c_str();
}


static kp_board* buildWith( const std::function<void( kp_board& )>& aLoader, bool aConnectivity, char* error,
                            size_t error_size );

static kp_board* buildBoard( const char* data, size_t length, char* error, size_t error_size )
{
    if( !data || length == 0 )
    {
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    return buildWith( [&]( kp_board& aBoard ) { kp::LoadBoard( aBoard.board, data, length ); }, true, error, error_size );
}

extern "C" kp_board* kp_board_load( const char* data, size_t length, char* error, size_t error_size )
{
    return buildBoard( data, length, error, error_size );
}


struct kp_fab_input
{
    std::vector<kp::FabInput> files;
};

extern "C" int32_t kp_data_is_fab( const char* data, size_t length )
{
    if( !data || length == 0 )
        return 0;

    return kp::LooksLikeGerber( data, length ) || kp::LooksLikeExcellon( data, length ) || kp::LooksLikeZip( data, length )
                   ? 1
                   : 0;
}

extern "C" kp_fab_input* kp_fab_input_new( void )
{
    return new( std::nothrow ) kp_fab_input();
}

extern "C" void kp_fab_input_add( kp_fab_input* input, const char* name, const char* data, size_t length )
{
    if( !input || !data )
        return;

    try
    {
        input->files.push_back( { name ? name : "", std::string( data, length ) } );
    }
    catch( const std::bad_alloc& )
    {
    }
}

extern "C" void kp_fab_input_free( kp_fab_input* input )
{
    delete input;
}

extern "C" kp_board* kp_board_load_fab( const kp_fab_input* input, char* error, size_t error_size )
{
    if( !input || input->files.empty() )
    {
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    return buildWith( [&]( kp_board& aBoard ) { kp::LoadFabFiles( aBoard.board, input->files ); }, false, error,
                      error_size );
}

extern "C" int32_t kp_board_is_fab( const kp_board* board )
{
    return board && !board->sch && board->board.generator == "gerber" ? 1 : 0;
}


extern "C" int32_t kp_board_kind( const kp_board* board )
{
    if( !board )
        return KP_KIND_PCB;

    if( board->sch )
        return KP_KIND_SCHEMATIC;

    return board->board.generator == "gerber" ? KP_KIND_GERBER : KP_KIND_PCB;
}


extern "C" void kp_board_background( const kp_board* board, float* rgba )
{
    if( !rgba )
        return;

    if( board && board->sch )
    {
        // KiCad's default schematic paper colour
        rgba[0] = 245 / 255.0f;
        rgba[1] = 244 / 255.0f;
        rgba[2] = 239 / 255.0f;
        rgba[3] = 1.0f;
        return;
    }

    for( int i = 0; i < 4; ++i )
        rgba[i] = kp::BACKGROUND_COLOR[(size_t) i];
}


extern "C" int32_t kp_data_is_schematic( const char* data, size_t length )
{
    return data && kp::LooksLikeSchematic( data, length ) ? 1 : 0;
}


static kp_board* buildSheet( std::shared_ptr<const kp::Schematic> aSch, int32_t aSheet, char* error,
                             size_t error_size )
{
    if( aSheet < 0 || aSheet >= (int32_t) aSch->sheets.size() )
        aSheet = 0;

    return buildWith(
            [&]( kp_board& b )
            {
                b.sch = aSch;
                b.sheet = aSheet;
                b.board = kp::Board();
                b.board.nets.push_back( std::string() );
            },
            false, error, error_size );
}


extern "C" int32_t kp_sch_guess_root( const kp_fab_input* files )
{
    if( !files )
        return -1;

    auto lower = []( std::string s )
    {
        for( char& c : s )
            c = (char) std::tolower( (unsigned char) c );

        return s;
    };

    auto base = [&]( const std::string& path )
    {
        size_t slash = path.find_last_of( "/\\" );
        return lower( slash == std::string::npos ? path : path.substr( slash + 1 ) );
    };

    auto stem = [&]( const std::string& path )
    {
        std::string b = base( path );
        size_t      dot = b.rfind( '.' );
        return dot == std::string::npos ? b : b.substr( 0, dot );
    };

    std::vector<int>      candidates;
    std::set<std::string> referenced, projects;
    std::vector<int>      refCount( files->files.size(), 0 );

    for( size_t i = 0; i < files->files.size(); ++i )
    {
        const kp::FabInput& f = files->files[i];
        std::string         b = base( f.name );

        if( b.size() > 10 && b.compare( b.size() - 10, 10, ".kicad_pro" ) == 0 )
            projects.insert( stem( f.name ) );

        if( !kp::LooksLikeSchematic( f.data.data(), f.data.size() ) )
            continue;

        candidates.push_back( (int) i );

        // sheet files this one refers to: (property "Sheetfile" "x.kicad_sch") or "Sheet file"
        for( const char* key : { "\"Sheetfile\"", "\"Sheet file\"" } )
        {
            for( size_t p = f.data.find( key ); p != std::string::npos; p = f.data.find( key, p + 1 ) )
            {
                size_t q1 = f.data.find( '"', p + std::strlen( key ) );
                size_t q2 = q1 == std::string::npos ? q1 : f.data.find( '"', q1 + 1 );

                if( q2 != std::string::npos && q2 - q1 < 1024 )
                {
                    referenced.insert( base( f.data.substr( q1 + 1, q2 - q1 - 1 ) ) );
                    refCount[i]++;
                }
            }
        }
    }

    if( candidates.empty() )
        return -1;

    int best = -1;

    for( int c : candidates )
    {
        const std::string& name = files->files[(size_t) c].name;

        if( referenced.count( base( name ) ) )
            continue;

        // the project's own schematic first, then the one with the most sub-sheets
        auto score = [&]( int i ) { return std::make_pair( projects.count( stem( files->files[(size_t) i].name ) ) ? 1 : 0, refCount[(size_t) i] ); };

        if( best < 0 || score( c ) > score( best ) )
            best = c;
    }

    return best >= 0 ? best : candidates.front();
}


extern "C" kp_board* kp_board_load_sch( const kp_fab_input* files, int32_t root, int32_t sheet, char* error,
                                        size_t error_size )
{
    if( !files || files->files.empty() || root < 0 || root >= (int32_t) files->files.size() )
    {
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    auto sch = std::make_shared<kp::Schematic>();

    try
    {
        std::vector<kp::SchFile> in;

        for( const kp::FabInput& f : files->files )
            in.push_back( { f.name, f.data } );

        kp::LoadSchematic( *sch, in, (size_t) root );
    }
    catch( const kp::ParseError& e )
    {
        copyError( error, error_size, kp::TrF( "The file could not be read: {0}", { e.what() } ) );
        return nullptr;
    }
    catch( const std::bad_alloc& )
    {
        copyError( error, error_size, kp::Tr( "Out of memory (the board is too large)" ) );
        return nullptr;
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, kp::TrF( "Error: {0}", { e.what() } ) );
        return nullptr;
    }

    return buildSheet( sch, sheet, error, error_size );
}


extern "C" kp_board* kp_board_sch_open_sheet( const kp_board* board, int32_t sheet, char* error, size_t error_size )
{
    if( !board || !board->sch )
    {
        copyError( error, error_size, kp::Tr( "Not a schematic" ) );
        return nullptr;
    }

    return buildSheet( board->sch, sheet, error, error_size );
}


extern "C" int32_t kp_sch_sheet_count( const kp_board* board )
{
    return board && board->sch ? (int32_t) board->sch->sheets.size() : 0;
}


extern "C" int32_t kp_sch_current_sheet( const kp_board* board )
{
    return board && board->sch ? board->sheet : -1;
}


extern "C" void kp_sch_sheet_info( const kp_board* board, int32_t sheet, kp_sheet_info* out )
{
    if( !out )
        return;

    *out = kp_sheet_info { -1, -1, 0, 0 };

    if( !board || !board->sch || sheet < 0 || sheet >= (int32_t) board->sch->sheets.size() )
        return;

    const kp::SheetNode& n = board->sch->sheets[(size_t) sheet];
    out->parent = n.parent;
    out->sheet_item = n.sheetItem;
    out->child_count = (int32_t) n.children.size();
    out->depth = 0;

    for( int p = n.parent; p >= 0; p = board->sch->sheets[(size_t) p].parent )
        ++out->depth;
}


extern "C" const char* kp_sch_sheet_name( const kp_board* board, int32_t sheet )
{
    if( !board || !board->sch || sheet < 0 || sheet >= (int32_t) board->sch->sheets.size() )
        return "";

    return board->sch->sheets[(size_t) sheet].name.c_str();
}


extern "C" const char* kp_sch_sheet_page( const kp_board* board, int32_t sheet )
{
    if( !board || !board->sch || sheet < 0 || sheet >= (int32_t) board->sch->sheets.size() )
        return "";

    return board->sch->sheets[(size_t) sheet].page.c_str();
}


extern "C" const char* kp_sch_sheet_file( const kp_board* board, int32_t sheet )
{
    if( !board || !board->sch || sheet < 0 || sheet >= (int32_t) board->sch->sheets.size() )
        return "";

    const kp::SheetNode& n = board->sch->sheets[(size_t) sheet];
    return n.screen >= 0 ? board->sch->screens[(size_t) n.screen].fileName.c_str() : "";
}


extern "C" int32_t kp_sch_sheet_for_item( const kp_board* board, int32_t sheet_item )
{
    if( !board || !board->sch )
        return -1;

    for( int c : board->sch->sheets[(size_t) board->sheet].children )
    {
        if( board->sch->sheets[(size_t) c].sheetItem == sheet_item )
            return c;
    }

    return -1;
}


extern "C" int32_t kp_sch_missing_count( const kp_board* board )
{
    return board && board->sch ? (int32_t) board->sch->missingFiles.size() : 0;
}


extern "C" const char* kp_sch_missing_file( const kp_board* board, int32_t index )
{
    if( !board || !board->sch || index < 0 || index >= (int32_t) board->sch->missingFiles.size() )
        return "";

    return board->sch->missingFiles[(size_t) index].c_str();
}


static kp_board* buildWith( const std::function<void( kp_board& )>& aLoader, bool aConnectivity, char* error,
                            size_t error_size )
{
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

        aLoader( *b );

        auto t1 = clock::now();

        if( b->sch )
            kp::BuildSchematicMesh( *b->sch, b->sheet, b->mesh );
        else
            kp::BuildMesh( b->board, b->mesh );

        auto t2 = clock::now();

        kp::Connectivity conn;

        if( aConnectivity )
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

        if( b->sch )
        {
            // schematic: symbols, pins, size of the page
            const kp::SheetNode& node = b->sch->sheets[(size_t) b->sheet];
            const kp::SchScreen& sc = b->sch->screens[(size_t) node.screen];
            o.footprints = (int32_t) sc.symbols.size();
            o.pads = kp::SchematicPinBase( *b->sch, sc ).back();
            o.tracks = 0;

            for( const kp::SchLine& l : sc.lines )
                o.tracks += l.kind == kp::SchLine::WIRE ? 1 : 0;

            o.graphics = (int32_t) ( sc.graphics.size() + sc.texts.size() );
            o.texts = (int32_t) sc.labels.size();
            o.copper_layers = 0;
            o.nets = 0;

            for( const kp::SchNet& n : b->sch->nets )
                o.nets += n.bus ? 0 : 1;

            o.width_mm = sc.paperW;
            o.height_mm = sc.paperH;
            o.file_version = sc.version;
            o.warning_count = (int32_t) b->sch->warnings.size();
            const kp::TitleBlock& t = b->sch->screens[0].titleBlock;
            b->board.generator = sc.generator;
            b->board.generatorVersion = sc.generatorVersion;
            b->board.titleBlock["TITLE"] = t.title;
            b->board.titleBlock["REVISION"] = t.rev;
            b->board.titleBlock["COMPANY"] = t.company;
            b->board.titleBlock["ISSUE_DATE"] = t.date;
            b->board.warnings = b->sch->warnings;
        }

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
    // schematic sheets are picked by their triangles, so they keep the mesh
    if( !board || board->sch )
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

    if( board->sch )
    {
        kp::SchPick( *board->sch, board->sheet, board->mesh, const_cast<kp_board*>( board )->schPick, kp::Pt( x, y ),
                     tolerance, makeFilter( slot_visible, slot_count, hidden_kinds, flipped ), hits );

        for( kp::PickHit& h : hits )
            h.net = h.net + 1; // 0 = none, as for boards
    }
    else
    {
        kp::Pick( board->board, board->mesh, kp::Pt( x, y ), tolerance,
                  makeFilter( slot_visible, slot_count, hidden_kinds, flipped ), hits );
    }

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

    if( board->sch )
        board->infoScratch = kp::SchItemInfo( *board->sch, board->sheet, (kp::ItemType) item->type, item->index );
    else
        board->infoScratch = kp::ItemInfo( board->board, board->mesh, (kp::ItemType) item->type, item->index );

    return board->infoScratch.c_str();
}


extern "C" const char* kp_board_net_info( kp_board* board, int32_t net )
{
    if( !board )
        return "";

    if( board->sch )
        board->infoScratch = kp::SchNetInfo( *board->sch, board->sheet, net - 1 );
    else
        board->infoScratch = kp::NetInfo( board->board, net );

    return board->infoScratch.c_str();
}


extern "C" int32_t kp_board_item_bounds( const kp_board* board, const kp_item* item, int32_t whole_net,
                                         double* min_x, double* min_y, double* max_x, double* max_y )
{
    if( !board || !item )
        return 0;

    kp::BBox b;

    if( board->sch )
        b = whole_net ? kp::SchNetBounds( *board->sch, board->sheet, board->mesh, item->net - 1 )
                      : kp::SchItemBounds( *board->sch, board->sheet, board->mesh, (kp::ItemType) item->type, item->index );
    else
        b = whole_net ? kp::NetBounds( board->board, item->net )
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
        if( board->sch )
            kp::SchHighlightRanges( *board->sch, board->sheet, board->mesh, (kp::ItemType) item->type, item->index,
                                    whole_net != 0, item->net - 1, board->rangeScratch );
        else
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


static const kp::SchScreen* schScreen( const kp_board* board )
{
    if( !board || !board->sch || board->sheet < 0 || board->sheet >= (int) board->sch->sheets.size() )
        return nullptr;

    int screen = board->sch->sheets[(size_t) board->sheet].screen;
    return screen < 0 ? nullptr : &board->sch->screens[(size_t) screen];
}


/// References and values of the symbols on a schematic board's sheet.
static void schSheetSymbols( const kp_board* board )
{
    kp_board*            b = const_cast<kp_board*>( board );
    const kp::SchScreen* sc = schScreen( board );

    if( !sc || b->schSheetSymbolRefs.size() == sc->symbols.size() )
        return;

    const kp::SheetNode& node = board->sch->sheets[(size_t) board->sheet];
    b->schSheetSymbolRefs.clear();
    b->schSheetSymbolValues.clear();

    for( const kp::SchSymbol& sym : sc->symbols )
    {
        std::string value;

        for( const kp::SchField& f : sym.fields )
        {
            if( f.name == "Value" )
                value = kp::DisplayNetName( board->sch->FieldValue( node, sym, f ) );
        }

        b->schSheetSymbolRefs.push_back( kp::SchematicShownReference( *board->sch, node, sym ) );
        b->schSheetSymbolValues.push_back( value );
    }
}


extern "C" int32_t kp_board_footprint_count( const kp_board* board )
{
    if( const kp::SchScreen* sc = schScreen( board ) )
        return (int32_t) sc->symbols.size();

    return board ? (int32_t) board->board.footprints.size() : 0;
}


extern "C" const char* kp_board_footprint_ref( const kp_board* board, int32_t index )
{
    if( board && board->sch )
    {
        schSheetSymbols( board );
        return index >= 0 && index < (int32_t) board->schSheetSymbolRefs.size() ? board->schSheetSymbolRefs[(size_t) index].c_str() : "";
    }

    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return "";

    return board->board.footprints[(size_t) index].reference.c_str();
}


extern "C" const char* kp_board_footprint_value( const kp_board* board, int32_t index )
{
    if( board && board->sch )
    {
        schSheetSymbols( board );
        return index >= 0 && index < (int32_t) board->schSheetSymbolValues.size() ? board->schSheetSymbolValues[(size_t) index].c_str() : "";
    }

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
    if( board && board->sch )
        return (int32_t) board->sch->nets.size() + 1;

    return board ? (int32_t) board->board.nets.size() : 0;
}


extern "C" const char* kp_board_net_name( const kp_board* board, int32_t net )
{
    kp_board* b = const_cast<kp_board*>( board );

    if( board && board->sch )
    {
        if( net <= 0 || net > (int32_t) board->sch->nets.size() )
            return "";

        if( b->netDisplay.size() != board->sch->nets.size() + 1 )
        {
            b->netDisplay.assign( 1, std::string() );

            for( const kp::SchNet& n : board->sch->nets )
                b->netDisplay.push_back( kp::DisplayNetName( n.name ) );
        }

        return b->netDisplay[(size_t) net].c_str();
    }

    if( !board || net < 0 || net >= (int32_t) board->board.nets.size() )
        return "";

    if( b->netDisplay.size() != b->board.nets.size() )
    {
        b->netDisplay.clear();

        for( const std::string& n : b->board.nets )
            b->netDisplay.push_back( kp::DisplayNetName( n ) );
    }

    return b->netDisplay[(size_t) net].c_str();
}


extern "C" int32_t kp_board_footprint_pose( const kp_board* board, int32_t index, double* x, double* y,
                                            double* angle )
{
    if( const kp::SchScreen* sc = schScreen( board ) )
    {
        if( index < 0 || index >= (int32_t) sc->symbols.size() )
            return 0;

        const kp::SchSymbol& sym = sc->symbols[(size_t) index];

        if( x )
            *x = sym.pos.x;
        if( y )
            *y = sym.pos.y;
        if( angle )
            *angle = 0;

        return 1;
    }

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
    if( const kp::SchScreen* sc = schScreen( board ) )
    {
        if( index < 0 || index >= (int32_t) sc->symbols.size() )
            return 0;

        const kp::LibSymbol* lib = board->sch->Lib( *sc, sc->symbols[(size_t) index] );
        return lib ? (int32_t) lib->pins.size() : 0;
    }

    if( !board || index < 0 || index >= (int32_t) board->board.footprints.size() )
        return 0;

    return board->board.footprints[(size_t) index].padCount;
}


extern "C" int32_t kp_board_net_pad_count( const kp_board* board, int32_t net )
{
    if( board && board->sch )
        return net > 0 && net <= (int32_t) board->sch->nets.size() ? board->sch->nets[(size_t) net - 1].pinCount : 0;

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

    if( board->sch )
    {
        for( size_t i = 0; i < board->sch->nets.size(); ++i )
        {
            const std::string& n = board->sch->nets[i].name;

            if( n == name || kp::DisplayNetName( n ) == name )
                return (int32_t) i + 1;
        }

        return -1;
    }

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


// ------------------------------------------------------------------------------------------
// Schematic document
// ------------------------------------------------------------------------------------------

extern "C" kp_sch_doc* kp_sch_doc_create( const kp_fab_input* files, int32_t root, char* error, size_t error_size )
{
    if( !files || files->files.empty() || root < 0 || root >= (int32_t) files->files.size() )
    {
        copyError( error, error_size, kp::Tr( "The file is empty" ) );
        return nullptr;
    }

    kp_sch_doc* d = new( std::nothrow ) kp_sch_doc();

    if( !d )
    {
        copyError( error, error_size, kp::Tr( "Out of memory" ) );
        return nullptr;
    }

    try
    {
        std::vector<kp::SchFile> in;

        for( const kp::FabInput& f : files->files )
            in.push_back( { f.name, f.data } );

        d->doc.Load( in, (size_t) root );
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, kp::TrF( "The file could not be read: {0}", { e.what() } ) );
        delete d;
        return nullptr;
    }

    return d;
}


extern "C" void kp_sch_doc_free( kp_sch_doc* doc )
{
    delete doc;
}


extern "C" kp_board* kp_board_load_sch_doc( kp_sch_doc* doc, int32_t sheet, char* error, size_t error_size )
{
    if( !doc )
    {
        copyError( error, error_size, kp::Tr( "No document" ) );
        return nullptr;
    }

    try
    {
        if( !doc->cache || doc->cache->state != doc->doc.StateId() )
            doc->cache = doc->doc.Build();
    }
    catch( const kp::ParseError& e )
    {
        copyError( error, error_size, kp::TrF( "The file could not be read: {0}", { e.what() } ) );
        return nullptr;
    }
    catch( const std::bad_alloc& )
    {
        copyError( error, error_size, kp::Tr( "Out of memory" ) );
        return nullptr;
    }
    catch( const std::exception& e )
    {
        copyError( error, error_size, kp::TrF( "Error: {0}", { e.what() } ) );
        return nullptr;
    }

    std::shared_ptr<const kp::Schematic> sch( doc->cache, &doc->cache->sch );
    kp_board*                            b = buildSheet( sch, sheet, error, error_size );

    if( b )
    {
        b->schDoc = doc->cache;
        b->docOwner = doc;
        b->docState = doc->cache->state;
    }

    return b;
}


extern "C" int32_t kp_sch_doc_board_current( const kp_sch_doc* doc, const kp_board* board )
{
    return doc && board && board->schDoc && board->docOwner == doc && board->docState == doc->doc.StateId() ? 1 : 0;
}


extern "C" int32_t kp_sch_doc_file_count( const kp_sch_doc* doc )
{
    return doc ? (int32_t) doc->doc.FileCount() : 0;
}


extern "C" const char* kp_sch_doc_file_name( const kp_sch_doc* doc, int32_t index )
{
    if( !doc || index < 0 || index >= (int32_t) doc->doc.FileCount() )
        return "";

    return doc->doc.FileName( (size_t) index ).c_str();
}


extern "C" const char* kp_sch_doc_file_text( kp_sch_doc* doc, int32_t index, size_t* length )
{
    if( !doc || index < 0 || index >= (int32_t) doc->doc.FileCount() )
    {
        if( length )
            *length = 0;

        return "";
    }

    doc->textScratch = doc->doc.FileText( (size_t) index );

    if( length )
        *length = doc->textScratch.size();

    return doc->textScratch.c_str();
}


extern "C" int32_t kp_sch_doc_file_modified( const kp_sch_doc* doc, int32_t index )
{
    return doc && index >= 0 && index < (int32_t) doc->doc.FileCount() && doc->doc.FileModified( (size_t) index ) ? 1 : 0;
}


extern "C" int32_t kp_sch_doc_modified( const kp_sch_doc* doc ) { return doc && doc->doc.Modified() ? 1 : 0; }
extern "C" void    kp_sch_doc_mark_saved( kp_sch_doc* doc ) { if( doc ) doc->doc.MarkSaved(); }
extern "C" int32_t kp_sch_doc_can_undo( const kp_sch_doc* doc ) { return doc && doc->doc.CanUndo() ? 1 : 0; }
extern "C" int32_t kp_sch_doc_can_redo( const kp_sch_doc* doc ) { return doc && doc->doc.CanRedo() ? 1 : 0; }
extern "C" int32_t kp_sch_doc_undo( kp_sch_doc* doc ) { return doc && doc->doc.Undo() ? 1 : 0; }
extern "C" int32_t kp_sch_doc_redo( kp_sch_doc* doc ) { return doc && doc->doc.Redo() ? 1 : 0; }


extern "C" const char* kp_sch_doc_undo_label( kp_sch_doc* doc )
{
    if( !doc )
        return "";

    doc->labelScratch = doc->doc.UndoLabel();
    return doc->labelScratch.c_str();
}


extern "C" const char* kp_sch_doc_redo_label( kp_sch_doc* doc )
{
    if( !doc )
        return "";

    doc->labelScratch = doc->doc.RedoLabel();
    return doc->labelScratch.c_str();
}


template <typename F>
static int32_t runSchEdit( kp_sch_doc* doc, const kp_board* board, char* error, size_t error_size, F&& op )
{
    if( !doc || !board )
    {
        copyError( error, error_size, kp::Tr( "No document" ) );
        return 0;
    }

    if( !kp_sch_doc_board_current( doc, board ) )
    {
        copyError( error, error_size, kp::Tr( "The schematic view is not up to date yet; wait a moment and try again" ) );
        return 0;
    }

    try
    {
        kp::SchEditTarget t { doc->doc, *board->schDoc, board->sheet };
        std::string       err;

        if( !op( t, err ) )
        {
            doc->doc.Cancel();
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


static std::vector<kp::SchItemRef> schItems( const kp_item* items, int32_t count )
{
    std::vector<kp::SchItemRef> out;

    for( int32_t i = 0; items && i < count; ++i )
        out.push_back( { (kp::ItemType) items[i].type, items[i].index } );

    return out;
}


extern "C" int32_t kp_sch_edit_move( kp_sch_doc* doc, const kp_board* board, const kp_item* items, int32_t count,
                                     double dx, double dy, char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchMoveItems( t, schItems( items, count ), dx, dy, err ); } );
}


extern "C" int32_t kp_sch_edit_rotate( kp_sch_doc* doc, const kp_board* board, const kp_item* item, int32_t ccw,
                                       char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       {
                           std::vector<kp::SchItemRef> it = schItems( item, 1 );
                           return !it.empty() && kp::SchRotateItem( t, it[0], ccw != 0, err );
                       } );
}


extern "C" int32_t kp_sch_edit_mirror( kp_sch_doc* doc, const kp_board* board, const kp_item* item, int32_t vertical,
                                       char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       {
                           std::vector<kp::SchItemRef> it = schItems( item, 1 );
                           return !it.empty() && kp::SchMirrorItem( t, it[0], vertical != 0, err );
                       } );
}


extern "C" int32_t kp_sch_edit_delete( kp_sch_doc* doc, const kp_board* board, const kp_item* items, int32_t count,
                                       char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchDeleteItems( t, schItems( items, count ), err ); } );
}


extern "C" int32_t kp_sch_edit_set_field( kp_sch_doc* doc, const kp_board* board, int32_t symbol, const char* name,
                                          const char* value, char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchSetField( t, symbol, name ? name : "", value ? value : "", err ); } );
}


extern "C" int32_t kp_sch_edit_add_wire( kp_sch_doc* doc, const kp_board* board, const double* xy, int32_t points,
                                         int32_t bus, char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       {
                           std::vector<kp::Pt> pts;

                           for( int32_t i = 0; xy && i < points; ++i )
                               pts.emplace_back( xy[2 * i], xy[2 * i + 1] );

                           return kp::SchAddWire( t, pts, bus != 0, err );
                       } );
}


extern "C" int32_t kp_sch_edit_add_label( kp_sch_doc* doc, const kp_board* board, double x, double y, const char* text,
                                          int32_t kind, double angle, char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       {
                           kp::SchLabelKind k = kind == KP_LABEL_GLOBAL         ? kp::SchLabelKind::GLOBAL
                                                : kind == KP_LABEL_HIERARCHICAL ? kp::SchLabelKind::HIERARCHICAL
                                                                                : kp::SchLabelKind::LOCAL;
                           return kp::SchAddLabel( t, kp::Pt( x, y ), text ? text : "", k, angle, err );
                       } );
}


extern "C" int32_t kp_sch_edit_add_no_connect( kp_sch_doc* doc, const kp_board* board, double x, double y, char* error,
                                               size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchAddNoConnect( t, kp::Pt( x, y ), err ); } );
}


extern "C" int32_t kp_sch_edit_add_junction( kp_sch_doc* doc, const kp_board* board, double x, double y, char* error,
                                             size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchAddJunction( t, kp::Pt( x, y ), err ); } );
}


extern "C" int32_t kp_sch_edit_duplicate( kp_sch_doc* doc, const kp_board* board, int32_t symbol, double dx, double dy,
                                          char* error, size_t error_size )
{
    return runSchEdit( doc, board, error, error_size,
                       [&]( kp::SchEditTarget& t, std::string& err )
                       { return kp::SchDuplicateSymbol( t, symbol, dx, dy, err ); } );
}


extern "C" int32_t kp_sch_snap( const kp_board* board, double x, double y, double radius, double* out_x, double* out_y )
{
    kp::Pt out( x, y );
    bool   hit = false;

    if( board && board->sch )
        hit = kp::SchSnapPoint( *board->sch, board->sheet, kp::Pt( x, y ), radius, out );

    if( out_x )
        *out_x = out.x;

    if( out_y )
        *out_y = out.y;

    return hit ? 1 : 0;
}


extern "C" int32_t kp_sch_wire_from_stroke( const kp_board* board, const double* xy, int32_t points, double radius,
                                            double* out_xy, int32_t max_points )
{
    if( !board || !board->sch || !xy || points < 2 )
        return 0;

    std::vector<kp::Pt> stroke;

    for( int32_t i = 0; i < points; ++i )
        stroke.emplace_back( xy[2 * i], xy[2 * i + 1] );

    std::vector<kp::Pt> wire = kp::SchWireFromStroke( *board->sch, board->sheet, stroke, radius );

    for( size_t i = 0; out_xy && i < wire.size() && (int32_t) i < max_points; ++i )
    {
        out_xy[2 * i] = wire[i].x;
        out_xy[2 * i + 1] = wire[i].y;
    }

    return (int32_t) wire.size();
}


extern "C" int32_t kp_sch_pin_point( const kp_board* board, int32_t pin, double* x, double* y, int32_t* symbol )
{
    const kp::SchScreen* sc = schScreen( board );

    if( !sc || pin < 0 )
        return 0;

    std::vector<int> base = kp::SchematicPinBase( *board->sch, *sc );

    for( size_t s = 0; s + 1 < base.size(); ++s )
    {
        if( pin < base[s] || pin >= base[s + 1] )
            continue;

        const kp::SchSymbol& sym = sc->symbols[s];
        const kp::LibSymbol* lib = board->sch->Lib( *sc, sym );
        const kp::LibPin&    lp = lib->pins[(size_t) ( pin - base[s] )];
        const int            unit = board->sch->Unit( board->sch->sheets[(size_t) board->sheet], sym );
        kp::Pt               tip, root;
        kp::SchematicPinPoints( sym, lp, tip, root );

        if( x )
            *x = tip.x;

        if( y )
            *y = tip.y;

        if( symbol )
            *symbol = (int32_t) s;

        bool used = ( lp.unit == 0 || unit == 0 || lp.unit == unit )
                    && ( lib->bodyStyleCount <= 1 || lp.bodyStyle == 0 || lp.bodyStyle == sym.bodyStyle );
        return used ? 1 : 0;
    }

    return 0;
}


static const std::vector<std::pair<std::string, std::string>>& schFieldsOf( kp_board* board, int32_t symbol )
{
    if( board->schFieldsSymbol != symbol )
    {
        board->schFields = kp::SchSymbolFields( *board->sch, board->sheet, symbol );
        board->schFieldsSymbol = symbol;
    }

    return board->schFields;
}


extern "C" int32_t kp_sch_symbol_field_count( kp_board* board, int32_t symbol )
{
    return board && board->sch ? (int32_t) schFieldsOf( board, symbol ).size() : 0;
}


extern "C" const char* kp_sch_symbol_field_name( kp_board* board, int32_t symbol, int32_t index )
{
    if( !board || !board->sch )
        return "";

    const auto& f = schFieldsOf( board, symbol );
    return index >= 0 && index < (int32_t) f.size() ? f[(size_t) index].first.c_str() : "";
}


extern "C" const char* kp_sch_symbol_field_value( kp_board* board, int32_t symbol, int32_t index )
{
    if( !board || !board->sch )
        return "";

    const auto& f = schFieldsOf( board, symbol );
    return index >= 0 && index < (int32_t) f.size() ? f[(size_t) index].second.c_str() : "";
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
                                                    ? kp::TrF( "Draw track ({0})", { kp::DisplayNetName( board->board.nets[(size_t) route->result.net] ) } )
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
    return "0.6.0";
}
