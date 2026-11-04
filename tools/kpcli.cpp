/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Desktop test tool for the core. It uses only the public C API (the same one the iPad app
 * uses) and contains a small software rasteriser that mimics the Metal renderer (per-slot
 * stencil, straight alpha blending, label zoom thresholds, highlight dimming), so the output
 * PNG shows what the app will draw.
 *
 *   kpcli stats  <board>                    (a board may also be a Gerber / drill / zip file,
 *                                            or a folder of fabrication files)
 *   kpcli check  <board>...                 (exit code != 0 if any board fails to load)
 *   kpcli pick   <board> <x> <y> [tol]      (items under a point, with their descriptions)
 *   kpcli render <board> <out.png> [width] [options]
 *        --back                 view from the bottom side
 *        --view x0 y0 x1 y1     render only this window (mm)
 *        --pick x y             select the item at (x, y) and highlight it
 *        --net                  with --pick: highlight the item's whole net
 *        --no-labels            hide pad/net labels
 *        --no-ratsnest          hide airwires
 *   kpcli edit <in> <out> <op>...           apply edits, save, reload and verify
 *       ops: move REF DX DY | rotate REF DEG | delete REF | field REF NAME VALUE |
 *            width TRACK W | via VIA SIZE DRILL | del KIND INDEX | undo | redo
 *        ops: move REF DX DY | rotate REF DEG | delete REF | field REF NAME VALUE |
 *             width TRACK_INDEX W | undo | redo
 */
#include "kpcore.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

static bool readFile( const char* aPath, std::string& aOut )
{
    std::ifstream f( aPath, std::ios::binary );

    if( !f )
        return false;

    aOut.assign( std::istreambuf_iterator<char>( f ), std::istreambuf_iterator<char>() );
    return true;
}


static int g_sheet = 0; // schematic sheet to open (--sheet N)

/// A schematic (index 0) and every .kicad_sch file below its folder (candidates for its
/// sub-sheets), named relative to that folder.
static kp_fab_input* schematicInput( const char* aPath, int& aCount )
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        root = fs::absolute( aPath, ec );
    fs::path        dir = root.parent_path();
    kp_fab_input*   in = kp_fab_input_new();
    std::string     data;
    int&            count = aCount;
    count = 0;

    if( readFile( root.string().c_str(), data ) )
    {
        kp_fab_input_add( in, root.filename().string().c_str(), data.data(), data.size() );
        ++count;
    }

    for( auto it = fs::recursive_directory_iterator( dir, ec ); it != fs::recursive_directory_iterator(); it.increment( ec ) )
    {
        if( ec || it.depth() > 3 )
            continue;

        const fs::path& p = it->path();

        if( p.extension() != ".kicad_sch" || fs::equivalent( p, root, ec ) )
            continue;

        if( readFile( p.string().c_str(), data ) )
        {
            kp_fab_input_add( in, fs::relative( p, dir, ec ).generic_string().c_str(), data.data(), data.size() );
            ++count;
        }
    }

    return in;
}


/// A schematic and every .kicad_sch file below its folder (candidates for its sub-sheets).
static kp_board* loadSchematic( const char* aPath )
{
    int           count = 0;
    kp_fab_input* in = schematicInput( aPath, count );
    char          err[512] = { 0 };
    kp_board*     b = count ? kp_board_load_sch( in, 0, g_sheet, err, sizeof( err ) ) : nullptr;
    kp_fab_input_free( in );

    if( !b )
        fprintf( stderr, "%s: %s\n", aPath, err );

    return b;
}


/// The root sheet the app would pick among the schematic and project files below a folder.
static int cmdRoot( const char* aDir )
{
    namespace fs = std::filesystem;
    std::error_code          ec;
    kp_fab_input*            in = kp_fab_input_new();
    std::vector<std::string> names;
    std::string              data;

    for( auto it = fs::recursive_directory_iterator( aDir, ec ); it != fs::recursive_directory_iterator(); it.increment( ec ) )
    {
        if( ec || it.depth() > 3 )
            continue;

        const fs::path& p = it->path();

        if( ( p.extension() == ".kicad_sch" || p.extension() == ".kicad_pro" ) && readFile( p.string().c_str(), data ) )
        {
            names.push_back( fs::relative( p, aDir, ec ).generic_string() );
            kp_fab_input_add( in, names.back().c_str(), data.data(), data.size() );
        }
    }

    int root = kp_sch_guess_root( in );
    printf( "%s\n", root >= 0 ? names[(size_t) root].c_str() : "(none)" );
    kp_fab_input_free( in );
    return root >= 0 ? 0 : 1;
}


/// A .kicad_pcb, or fabrication files: one Gerber / drill / zip file, or a folder of them.
static kp_board* load( const char* aPath )
{
    std::string data;
    char        err[512] = { 0 };
    std::error_code ec;

    if( std::filesystem::is_directory( aPath, ec ) )
    {
        kp_fab_input* in = kp_fab_input_new();
        std::vector<std::filesystem::path> files;

        for( const auto& e : std::filesystem::directory_iterator( aPath, ec ) )
        {
            if( e.is_regular_file() )
                files.push_back( e.path() );
        }

        std::sort( files.begin(), files.end() );

        for( const auto& f : files )
        {
            if( readFile( f.string().c_str(), data ) )
                kp_fab_input_add( in, f.filename().string().c_str(), data.data(), data.size() );
        }

        kp_board* b = kp_board_load_fab( in, err, sizeof( err ) );
        kp_fab_input_free( in );

        if( !b )
            fprintf( stderr, "%s: %s\n", aPath, err );

        return b;
    }

    if( !readFile( aPath, data ) )
    {
        fprintf( stderr, "cannot read %s\n", aPath );
        return nullptr;
    }

    kp_board* b = nullptr;

    if( kp_data_is_schematic( data.data(), data.size() ) )
        return loadSchematic( aPath );

    if( kp_data_is_fab( data.data(), data.size() ) )
    {
        kp_fab_input* in = kp_fab_input_new();
        kp_fab_input_add( in, std::filesystem::path( aPath ).filename().string().c_str(), data.data(), data.size() );
        b = kp_board_load_fab( in, err, sizeof( err ) );
        kp_fab_input_free( in );
    }
    else
    {
        b = kp_board_load( data.data(), data.size(), err, sizeof( err ) );
    }

    if( !b )
        fprintf( stderr, "%s: %s\n", aPath, err );

    return b;
}


static int cmdStats( const char* aPath )
{
    kp_board* b = load( aPath );

    if( !b )
        return 1;

    kp_stats s;
    kp_board_stats( b, &s );
    printf( "file: %s\n", aPath );
    printf( "version: %d  generator: %s %s\n", s.file_version, kp_board_string( b, KP_STR_GENERATOR ),
            kp_board_string( b, KP_STR_GENERATOR_VERSION ) );
    printf( "title: %s\n", kp_board_string( b, KP_STR_TITLE ) );
    printf( "footprints: %d  pads: %d  tracks: %d  arcs: %d  vias: %d  zones: %d (+%d keepout)\n",
            s.footprints, s.pads, s.tracks, s.arcs, s.vias, s.zones, s.keepouts );
    printf( "nets: %d  copper layers: %d  graphics: %d  texts: %d  holes: %d\n", s.nets,
            s.copper_layers, s.graphics, s.texts, s.holes );
    printf( "size: %.2f x %.2f mm, thickness %.2f mm\n", s.width_mm, s.height_mm, s.thickness_mm );
    printf( "mesh: %lld vertices, %lld triangles, %d groups\n", (long long) s.vertex_count,
            (long long) s.triangle_count, kp_board_group_count( b ) );
    printf( "time: parse %.1f ms, mesh %.1f ms, connectivity %.1f ms\n", s.parse_ms, s.mesh_ms, s.conn_ms );
    printf( "unconnected (airwires): %d\n", s.unconnected );

    {
        size_t       nf = 0;
        const float* aw = kp_board_airwires( b, &nf );

        for( size_t k = 0; k + 3 < nf && k < 40; k += 4 )
            printf( "  airwire (%.3f, %.3f) -> (%.3f, %.3f)\n", aw[k], aw[k + 1], aw[k + 2], aw[k + 3] );
    }

    for( int i = 0; i < s.warning_count; ++i )
        printf( "warning: %s\n", kp_board_warning( b, i ) );

    printf( "slots:\n" );

    for( int i = 0; i < kp_board_slot_count( b ); ++i )
    {
        kp_slot_info si;
        kp_board_slot_info( b, i, &si );
        printf( "  %2d %-16s %-22s kind=%d side=%+d front=%d back=%d idx=%u parent=%d view=%+d\n", i,
                kp_board_slot_name( b, i ), kp_board_slot_display( b, i ), si.kind, si.side,
                si.sort_front, si.sort_back, si.index_count, si.parent_slot, si.view_side );
    }

    kp_board_free( b );
    return 0;
}


static void printItem( kp_board* b, const kp_item& it )
{
    static const char* names[] = { "none",       "pad",          "via",          "track",     "arc",
                                   "footprint",  "zone",         "shape",        "text",      "sch-symbol",
                                   "sch-pin",    "sch-wire",     "sch-junction", "sch-label", "sch-sheet",
                                   "sch-note",   "sch-sheet-pin" };
    const char* name = it.type >= 0 && it.type < (int) ( sizeof( names ) / sizeof( names[0] ) ) ? names[it.type] : "?";
    printf( "-- %s #%d (net %d, fp %d, layer %d)\n", name, it.index, it.net, it.footprint, it.layer );
    printf( "%s\n", kp_board_item_info( b, &it ) );
}


/// Pick at a grid of points on every sheet (or the board) and describe what is hit: a
/// robustness sweep for the pick / info / highlight code.
static int cmdPickSweep( const char* aPath, int aSteps )
{
    kp_board* root = load( aPath );

    if( !root )
        return 1;

    const int sheets = std::max( 1, (int) kp_sch_sheet_count( root ) );
    long      hits = 0, infos = 0, ranges = 0;

    for( int s = 0; s < sheets; ++s )
    {
        char      err[256] = "";
        kp_board* b = kp_sch_sheet_count( root ) ? kp_board_sch_open_sheet( root, s, err, sizeof( err ) ) : root;

        if( !b )
            continue;

        double x0, y0, x1, y1;
        kp_board_bounds( b, &x0, &y0, &x1, &y1 );

        for( int i = 0; i <= aSteps; ++i )
        {
            for( int j = 0; j <= aSteps; ++j )
            {
                double  x = x0 + ( x1 - x0 ) * i / aSteps, y = y0 + ( y1 - y0 ) * j / aSteps;
                kp_item items[8];
                int     n = kp_board_pick( b, x, y, 1.0, nullptr, 0, 0, 0, items, 8 );
                hits += n;

                for( int k = 0; k < n; ++k )
                {
                    infos += strlen( kp_board_item_info( b, &items[k] ) ) > 0;
                    ranges += kp_board_item_ranges( b, &items[k], 0, nullptr, 0 );
                    double a, c, d, e;
                    kp_board_item_bounds( b, &items[k], 0, &a, &c, &d, &e );

                    if( items[k].net > 0 )
                    {
                        ranges += kp_board_item_ranges( b, &items[k], 1, nullptr, 0 );
                        kp_board_item_bounds( b, &items[k], 1, &a, &c, &d, &e );
                        infos += strlen( kp_board_net_info( b, items[k].net ) ) > 0;
                        int32_t sh[4];
                        kp_sch_net_sheets( b, items[k].net, sh, 4 );
                    }
                }
            }
        }

        for( int f = 0; f < kp_board_footprint_count( b ); ++f )
        {
            double x, y, a;
            infos += kp_board_footprint_pose( b, f, &x, &y, &a ) && kp_board_footprint_ref( b, f )[0];
        }

        if( b != root )
            kp_board_free( b );
    }

    for( int i = 0; i < kp_sch_symbol_count( root ); ++i )
    {
        int32_t     sheet, index;
        const char *ref, *value;
        kp_sch_symbol_entry( root, i, &sheet, &index, &ref, &value );
    }

    printf( "%d sheet(s): %ld hits, %ld descriptions, %ld highlight ranges\n", sheets, hits, infos, ranges );
    kp_board_free( root );
    return 0;
}


static int cmdPick( const char* aPath, double x, double y, double tol )
{
    kp_board* b = load( aPath );

    if( !b )
        return 1;

    kp_item items[16];
    int     n = kp_board_pick( b, x, y, tol, nullptr, 0, 0, 0, items, 16 );
    printf( "%d item(s) at (%.3f, %.3f)\n", n, x, y );

    for( int i = 0; i < n; ++i )
        printItem( b, items[i] );

    if( n > 0 && items[0].net > 0 )
    {
        printf( "-- net\n%s\n", kp_board_net_info( b, items[0].net ) );
        int r1 = kp_board_item_ranges( b, &items[0], 0, nullptr, 0 );
        int r2 = kp_board_item_ranges( b, &items[0], 1, nullptr, 0 );
        printf( "highlight ranges: item=%d net=%d\n", r1, r2 );
    }

    kp_board_free( b );
    return 0;
}


struct Raster
{
    int                  w, h;
    double               x0, y0, scale; // world -> pixel
    bool                 back;
    double               mirrorX;       // world x mirror axis for bottom view
    std::vector<float>   color;
    std::vector<uint8_t> stencil;

    void toPx( double x, double y, double& px, double& py ) const
    {
        if( back )
            x = mirrorX - x;

        px = ( x - x0 ) * scale;
        py = ( y - y0 ) * scale;
    }

    void fill( const float* V, const uint32_t* I, uint32_t start, uint32_t count, uint8_t ref, float r,
               float g, float bl, float a )
    {
        for( uint32_t t = start; t + 2 < start + count + 0; t += 3 )
        {
            double ax, ay, bx, by, cx, cy;
            toPx( V[I[t] * 2], V[I[t] * 2 + 1], ax, ay );
            toPx( V[I[t + 1] * 2], V[I[t + 1] * 2 + 1], bx, by );
            toPx( V[I[t + 2] * 2], V[I[t + 2] * 2 + 1], cx, cy );

            double area = ( bx - ax ) * ( cy - ay ) - ( by - ay ) * ( cx - ax );

            if( std::fabs( area ) < 1e-12 )
                continue;

            int minx = std::max( 0, (int) std::floor( std::min( { ax, bx, cx } ) ) );
            int maxx = std::min( w - 1, (int) std::ceil( std::max( { ax, bx, cx } ) ) );
            int miny = std::max( 0, (int) std::floor( std::min( { ay, by, cy } ) ) );
            int maxy = std::min( h - 1, (int) std::ceil( std::max( { ay, by, cy } ) ) );

            for( int y = miny; y <= maxy; ++y )
            {
                double py = y + 0.5;

                for( int x = minx; x <= maxx; ++x )
                {
                    double px = x + 0.5;
                    double w0 = ( bx - ax ) * ( py - ay ) - ( by - ay ) * ( px - ax );
                    double w1 = ( cx - bx ) * ( py - by ) - ( cy - by ) * ( px - bx );
                    double w2 = ( ax - cx ) * ( py - cy ) - ( ay - cy ) * ( px - cx );

                    bool inside = area > 0 ? ( w0 >= 0 && w1 >= 0 && w2 >= 0 )
                                           : ( w0 <= 0 && w1 <= 0 && w2 <= 0 );

                    if( !inside )
                        continue;

                    size_t p = (size_t) y * w + x;

                    if( stencil[p] == ref )
                        continue;

                    stencil[p] = ref;
                    float* c = &color[p * 3];
                    c[0] = r * a + c[0] * ( 1 - a );
                    c[1] = g * a + c[1] * ( 1 - a );
                    c[2] = bl * a + c[2] * ( 1 - a );
                }
            }
        }
    }
};


static int cmdRender( int argc, char** argv )
{
    const char* path = argv[0];
    const char* outPath = argv[1];
    int         width = 1600;
    bool        back = false, haveView = false, havePick = false, wholeNet = false, labels = true;
    bool        ratsnest = true;
    double      vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0, pickX = 0, pickY = 0;

    for( int i = 2; i < argc; ++i )
    {
        std::string a = argv[i];

        if( a == "--back" )
            back = true;
        else if( a == "--net" )
            wholeNet = true;
        else if( a == "--no-labels" )
            labels = false;
        else if( a == "--no-ratsnest" )
            ratsnest = false;
        else if( a == "--sheet" && i + 1 < argc )
            g_sheet = atoi( argv[++i] );
        else if( a == "--view" && i + 4 < argc )
        {
            haveView = true;
            vx0 = atof( argv[++i] );
            vy0 = atof( argv[++i] );
            vx1 = atof( argv[++i] );
            vy1 = atof( argv[++i] );
        }
        else if( a == "--pick" && i + 2 < argc )
        {
            havePick = true;
            pickX = atof( argv[++i] );
            pickY = atof( argv[++i] );
        }
        else
            width = atoi( argv[i] );
    }

    kp_board* b = load( path );

    if( !b )
        return 1;

    double bx0, by0, bx1, by1;
    kp_board_bounds( b, &bx0, &by0, &bx1, &by1 );
    double x0 = bx0, y0 = by0, x1 = bx1, y1 = by1;

    if( haveView )
    {
        x0 = std::min( vx0, vx1 );
        x1 = std::max( vx0, vx1 );
        y0 = std::min( vy0, vy1 );
        y1 = std::max( vy0, vy1 );
    }
    else
    {
        double mw = ( x1 - x0 ) * 0.03 + 0.5, mh = ( y1 - y0 ) * 0.03 + 0.5;
        x0 -= mw;
        x1 += mw;
        y0 -= mh;
        y1 += mh;
    }

    const int SS = 2;
    int       W = width;
    int       H = std::max( 16, (int) std::lround( width * ( y1 - y0 ) / ( x1 - x0 ) ) );

    Raster R;
    R.w = W * SS;
    R.h = H * SS;
    R.scale = R.w / ( x1 - x0 );
    R.back = back;
    R.mirrorX = x0 + x1; // mirror the view window onto itself
    R.x0 = x0;
    R.y0 = y0;
    R.color.resize( (size_t) R.w * R.h * 3 );
    R.stencil.assign( (size_t) R.w * R.h, 0 );

    float bg[4];
    kp_board_background( b, bg );

    for( size_t i = 0; i < (size_t) R.w * R.h; ++i )
    {
        R.color[i * 3 + 0] = bg[0];
        R.color[i * 3 + 1] = bg[1];
        R.color[i * 3 + 2] = bg[2];
    }

    double zoom = W / ( x1 - x0 ); // output pixels per mm (what the app calls zoom)

    size_t          nv = 0, ni = 0;
    const float*    V = kp_board_vertices( b, &nv );
    const uint32_t* I = kp_board_indices( b, &ni );

    int                       nslots = kp_board_slot_count( b );
    std::vector<kp_slot_info> slots( (size_t) nslots );

    for( int i = 0; i < nslots; ++i )
        kp_board_slot_info( b, i, &slots[(size_t) i] );

    int                   ng = kp_board_group_count( b );
    std::vector<kp_group> groups( (size_t) ng );

    for( int i = 0; i < ng; ++i )
        kp_board_group( b, i, &groups[(size_t) i] );

    auto sortKey = [&]( int slot )
    { return back ? slots[(size_t) slot].sort_back : slots[(size_t) slot].sort_front; };

    std::stable_sort( groups.begin(), groups.end(),
                      [&]( const kp_group& a, const kp_group& c ) { return sortKey( a.slot ) < sortKey( c.slot ); } );

    auto slotShown = [&]( int slot ) -> bool
    {
        const kp_slot_info& s = slots[(size_t) slot];

        if( s.view_side != 0 && s.view_side != ( back ? -1 : 1 ) )
            return false;

        return true;
    };

    int drawnGroups = 0;

    for( const kp_group& g : groups )
    {
        bool isLabel = g.kind >= KP_KIND_LABEL0 && g.kind <= KP_KIND_LABEL4;

        if( !slotShown( g.slot ) || ( isLabel && ( !labels || zoom < g.min_zoom ) ) )
            continue;

        R.fill( V, I, g.index_start, g.index_count, (uint8_t) ( g.slot % 255 + 1 ), g.r, g.g, g.b, g.a );
        ++drawnGroups;
    }

    if( ratsnest )
    {
        size_t       nf = 0;
        const float* aw = kp_board_airwires( b, &nf );
        double       hw = 1.0 * SS / R.scale; // ~1 output pixel wide, in mm

        for( size_t k = 0; k + 3 < nf; k += 4 )
        {
            double ax = aw[k], ay = aw[k + 1], bx = aw[k + 2], by = aw[k + 3];
            double dx = bx - ax, dy = by - ay, len = std::hypot( dx, dy );

            if( len <= 0 )
                continue;

            double nx = -dy / len * hw, ny = dx / len * hw;
            float  quad[8] = { (float) ( ax + nx ), (float) ( ay + ny ), (float) ( ax - nx ), (float) ( ay - ny ),
                               (float) ( bx - nx ), (float) ( by - ny ), (float) ( bx + nx ), (float) ( by + ny ) };
            uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
            R.fill( quad, idx, 0, 6, 254, 0.0f, 248 / 255.0f, 1.0f, 0.75f );
        }
    }

    if( havePick )
    {
        kp_item items[8];
        int     n = kp_board_pick( b, pickX, pickY, 0.2, nullptr, 0, 0, back, items, 8 );

        if( n > 0 )
        {
            printf( "selected: %s\n", kp_board_item_info( b, &items[0] ) );

            // dim everything (and reset the stencil, like the full-screen quad in the app)
            for( size_t i = 0; i < (size_t) R.w * R.h; ++i )
            {
                R.color[i * 3 + 0] = R.color[i * 3 + 0] * 0.35f;
                R.color[i * 3 + 1] = R.color[i * 3 + 1] * 0.35f + ( 16 / 255.0f ) * 0.65f;
                R.color[i * 3 + 2] = R.color[i * 3 + 2] * 0.35f + ( 35 / 255.0f ) * 0.65f;
                R.stencil[i] = 0;
            }

            int                   total = kp_board_item_ranges( b, &items[0], wholeNet, nullptr, 0 );
            std::vector<kp_range> ranges( (size_t) total );
            kp_board_item_ranges( b, &items[0], wholeNet, ranges.data(), total );

            std::stable_sort( ranges.begin(), ranges.end(),
                              [&]( const kp_range& a, const kp_range& c ) { return sortKey( a.slot ) < sortKey( c.slot ); } );

            for( const kp_range& r : ranges )
            {
                const kp_slot_info& s = slots[(size_t) r.slot];
                float               mix = 0.45f;
                R.fill( V, I, r.index_start, r.index_count, (uint8_t) ( r.slot % 255 + 1 ),
                        s.r + ( 1 - s.r ) * mix, s.g + ( 1 - s.g ) * mix, s.b + ( 1 - s.b ) * mix,
                        std::max( s.a, 0.85f ) );
            }

            printf( "highlight: %d range(s)%s\n", total, wholeNet ? " (whole net)" : "" );
        }
        else
        {
            printf( "nothing at (%.3f, %.3f)\n", pickX, pickY );
        }
    }

    std::vector<uint8_t> img( (size_t) W * H * 3 );

    for( int y = 0; y < H; ++y )
    {
        for( int x = 0; x < W; ++x )
        {
            for( int ch = 0; ch < 3; ++ch )
            {
                float sum = 0;

                for( int sy = 0; sy < SS; ++sy )
                    for( int sx = 0; sx < SS; ++sx )
                        sum += R.color[( (size_t) ( y * SS + sy ) * R.w + ( x * SS + sx ) ) * 3 + ch];

                img[( (size_t) y * W + x ) * 3 + ch] =
                        (uint8_t) std::clamp( (int) std::lround( sum / ( SS * SS ) * 255 ), 0, 255 );
            }
        }
    }

    int ok = stbi_write_png( outPath, W, H, 3, img.data(), W * 3 );
    printf( "%s: %dx%d (zoom %.1f px/mm), %d/%d groups, %zu triangles -> %s\n", path, W, H, zoom, drawnGroups,
            ng, ni / 3, ok ? outPath : "WRITE FAILED" );
    kp_board_free( b );
    return ok ? 0 : 1;
}


static int cmdCheck( int argc, char** argv )
{
    int failed = 0;

    for( int i = 0; i < argc; ++i )
    {
        kp_board* b = load( argv[i] );

        if( !b )
        {
            ++failed;
            continue;
        }

        kp_stats s;
        kp_board_stats( b, &s );

        // exercise picking and highlighting at a few points
        double x0, y0, x1, y1;
        kp_board_bounds( b, &x0, &y0, &x1, &y1 );
        int picks = 0, ranges = 0;

        for( int k = 0; k < 25; ++k )
        {
            double  px = x0 + ( x1 - x0 ) * ( ( k % 5 ) + 0.5 ) / 5;
            double  py = y0 + ( y1 - y0 ) * ( ( k / 5 ) + 0.5 ) / 5;
            kp_item it[4];
            int     n = kp_board_pick( b, px, py, 0.5, nullptr, 0, 0, k & 1, it, 4 );
            picks += n;

            for( int j = 0; j < n; ++j )
            {
                (void) kp_board_item_info( b, &it[j] );
                ranges += kp_board_item_ranges( b, &it[j], 0, nullptr, 0 );

                if( it[j].net > 0 )
                {
                    ranges += kp_board_item_ranges( b, &it[j], 1, nullptr, 0 );
                    (void) kp_board_net_info( b, it[j].net );
                }
            }
        }

        printf( "OK  v%-8d fp=%-5d pads=%-6d trk=%-6d via=%-5d zone=%-3d tri=%-8lld picks=%-3d unc=%-4d warn=%d  %.0f+%.0f+%.0f ms  %s\n",
                s.file_version, s.footprints, s.pads, s.tracks + s.arcs, s.vias, s.zones,
                (long long) s.triangle_count, picks, s.unconnected, s.warning_count, s.parse_ms, s.mesh_ms,
                s.conn_ms, argv[i] );
        (void) ranges;
        kp_board_free( b );
    }

    printf( "%d/%d loaded\n", argc - failed, argc );
    return failed ? 1 : 0;
}


static int findFootprint( const kp_board* b, const char* ref )
{
    for( int i = 0; i < kp_board_footprint_count( b ); ++i )
    {
        if( !strcmp( kp_board_footprint_ref( b, i ), ref ) )
            return i;
    }

    return -1;
}


static int cmdEdit( int argc, char** argv )
{
    const char* inPath = argv[0];
    const char* outPath = argv[1];
    std::string data;

    if( !readFile( inPath, data ) )
    {
        fprintf( stderr, "cannot read %s\n", inPath );
        return 1;
    }

    char    err[512] = { 0 };
    kp_doc* doc = kp_doc_create( data.data(), data.size(), err, sizeof( err ) );

    if( !doc )
    {
        fprintf( stderr, "%s\n", err );
        return 1;
    }

    kp_board* b = kp_board_load_doc( doc, err, sizeof( err ) );

    if( !b )
    {
        fprintf( stderr, "%s\n", err );
        return 1;
    }

    kp_stats s0;
    kp_board_stats( b, &s0 );
    int failures = 0;

    for( int i = 2; i < argc; )
    {
        std::string op = argv[i++];
        int         ok = 0;
        err[0] = 0;

        if( op == "move" && i + 2 < argc + 0 )
        {
            int fp = findFootprint( b, argv[i] );
            ok = kp_edit_move_footprint( doc, b, fp, atof( argv[i + 1] ), atof( argv[i + 2] ), err, sizeof( err ) );
            printf( "move %s %s %s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "rotate" && i + 1 < argc + 0 )
        {
            int fp = findFootprint( b, argv[i] );
            ok = kp_edit_rotate_footprint( doc, b, fp, atof( argv[i + 1] ), err, sizeof( err ) );
            printf( "rotate %s %s: %s %s\n", argv[i], argv[i + 1], ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "delete" && i < argc )
        {
            kp_item it { 5, findFootprint( b, argv[i] ), -1, 0, -1 };
            it.footprint = it.index;
            ok = kp_edit_delete( doc, b, &it, err, sizeof( err ) );
            printf( "delete %s: %s %s\n", argv[i], ok ? "ok" : "FAILED", err );
            i += 1;
        }
        else if( op == "field" && i + 2 < argc + 0 )
        {
            int fp = findFootprint( b, argv[i] );
            ok = kp_edit_set_field( doc, b, fp, argv[i + 1], argv[i + 2], err, sizeof( err ) );
            printf( "field %s %s=%s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "width" && i + 1 < argc + 0 )
        {
            kp_item it { 3, atoi( argv[i] ), -1, 0, -1 };
            ok = kp_edit_set_track_width( doc, b, &it, atof( argv[i + 1] ), err, sizeof( err ) );
            printf( "width #%s %s: %s %s\n", argv[i], argv[i + 1], ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "del" && i + 1 < argc + 0 )
        {
            // del <track|arc|via|zone|shape|text|pad> <index>
            static const std::pair<const char*, int> kinds[] = {
                { "pad", KP_ITEM_PAD },     { "via", KP_ITEM_VIA },     { "track", KP_ITEM_TRACK },
                { "arc", KP_ITEM_ARC },     { "zone", KP_ITEM_ZONE },   { "shape", KP_ITEM_SHAPE },
                { "text", KP_ITEM_TEXT }
            };
            int type = 0;

            for( const auto& k : kinds )
            {
                if( !strcmp( argv[i], k.first ) )
                    type = k.second;
            }

            kp_item it { type, atoi( argv[i + 1] ), -1, 0, -1 };
            ok = type && kp_edit_delete( doc, b, &it, err, sizeof( err ) );
            printf( "del %s #%s: %s %s\n", argv[i], argv[i + 1], ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "via" && i + 2 < argc + 0 )
        {
            ok = kp_edit_set_via( doc, b, atoi( argv[i] ), atof( argv[i + 1] ), atof( argv[i + 2] ), err, sizeof( err ) );
            printf( "via #%s %s/%s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "track" && i + 6 < argc + 0 )
        {
            // track X1 Y1 X2 Y2 WIDTH LAYER NET
            int slot = -1;

            for( int k = 0; k < kp_board_slot_count( b ); ++k )
            {
                if( !strcmp( kp_board_slot_name( b, k ), argv[i + 5] ) && kp_board_slot_layer( b, k ) >= 0 )
                    slot = k;
            }

            int net = kp_board_net_find( b, argv[i + 6] );
            ok = kp_edit_add_track( doc, b, atof( argv[i] ), atof( argv[i + 1] ), atof( argv[i + 2] ),
                                    atof( argv[i + 3] ), atof( argv[i + 4] ), slot, net < 0 ? 0 : net, err,
                                    sizeof( err ) );
            printf( "track %s,%s-%s,%s %s %s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], argv[i + 3],
                    argv[i + 5], argv[i + 6], ok ? "ok" : "FAILED", err );
            i += 7;
        }
        else if( op == "addvia" && i + 4 < argc + 0 )
        {
            // addvia X Y SIZE DRILL NET
            int net = kp_board_net_find( b, argv[i + 4] );
            ok = kp_edit_add_via( doc, b, atof( argv[i] ), atof( argv[i + 1] ), atof( argv[i + 2] ),
                                  atof( argv[i + 3] ), net < 0 ? 0 : net, err, sizeof( err ) );
            printf( "addvia %s,%s %s: %s %s\n", argv[i], argv[i + 1], argv[i + 4], ok ? "ok" : "FAILED", err );
            i += 5;
        }
        else if( op == "route" && i + 2 < argc + 0 )
        {
            // route LAYER WIDTH N x0 y0 ... (pencil stroke, N points), committed unless blocked
            int slot = -1;

            for( int k = 0; k < kp_board_slot_count( b ); ++k )
            {
                if( !strcmp( kp_board_slot_name( b, k ), argv[i] ) && kp_board_slot_layer( b, k ) >= 0 )
                    slot = k;
            }

            double             width = atof( argv[i + 1] );
            int                n = atoi( argv[i + 2] );
            std::vector<float> xy;
            i += 3;

            for( int k = 0; k < 2 * n && i < argc; ++k )
                xy.push_back( (float) atof( argv[i++] ) );

            kp_route* r = kp_route_build( b, xy.data(), (int32_t) xy.size() / 2, slot, width, 0.3, 0.5, 0.6, 0.3, 0.2 );
            kp_route_info info;
            kp_route_get_info( r, &info );
            static const char* kinds[] = { "none", "pad", "via", "track-end", "track-mid" };
            printf( "route %s w=%g: ok=%d blocked=%d net=%s start=%s%s end=%s%s tracks=%d vias=%d len=%.3f "
                    "shorts=%d close=%d msg='%s'\n",
                    argv[i - 2 * n - 3], width, info.ok, info.blocked,
                    info.net > 0 ? kp_board_net_name( b, info.net ) : "-", kinds[info.start_kind],
                    info.start_via ? "+via" : "", kinds[info.end_kind], info.end_via ? "+via" : "",
                    info.track_count, info.via_count, info.length, info.shorts, info.too_close, kp_route_message( r ) );
            size_t       cnt = 0;
            const float* t = kp_route_tracks( r, &cnt );

            for( size_t k = 0; k + 3 < cnt; k += 4 )
                printf( "  seg (%.6f, %.6f) -> (%.6f, %.6f)\n", t[k], t[k + 1], t[k + 2], t[k + 3] );

            const float* c = kp_route_conflicts( r, &cnt );

            for( size_t k = 0; k + 2 < cnt; k += 3 )
                printf( "  conflict (%.3f, %.3f) %s\n", c[k], c[k + 1], c[k + 2] > 0 ? "short" : "clearance" );

            ok = kp_edit_add_route( doc, b, r, err, sizeof( err ) );
            printf( "  commit: %s %s\n", ok ? "ok" : "FAILED", err );
            kp_route_free( r );
        }
        else if( op == "undo" )
        {
            ok = kp_doc_undo( doc );
            printf( "undo: %s\n", ok ? "ok" : "nothing to undo" );
        }
        else if( op == "redo" )
        {
            ok = kp_doc_redo( doc );
            printf( "redo: %s\n", ok ? "ok" : "nothing to redo" );
        }
        else
        {
            fprintf( stderr, "bad op %s\n", op.c_str() );
            return 2;
        }

        failures += !ok;

        // rebuild the board from the edited document, as the app does
        kp_board* nb = kp_board_load_doc( doc, err, sizeof( err ) );

        if( !nb )
        {
            fprintf( stderr, "rebuild failed: %s\n", err );
            return 1;
        }

        kp_board_free( b );
        b = nb;
    }

    size_t      len = 0;
    const char* text = kp_doc_text( doc, &len );
    std::ofstream out( outPath, std::ios::binary );
    out.write( text, (std::streamsize) len );
    out.close();

    kp_stats s1;
    kp_board_stats( b, &s1 );
    printf( "saved %s (%zu bytes, modified=%d). footprints %d -> %d, pads %d -> %d, unconnected %d -> %d\n",
            outPath, len, kp_doc_modified( doc ), s0.footprints, s1.footprints, s0.pads, s1.pads, s0.unconnected,
            s1.unconnected );

    // the saved file must load again
    kp_board* check = load( outPath );

    if( !check )
        return 1;

    kp_board_free( check );
    kp_board_free( b );
    kp_doc_free( doc );
    return failures ? 3 : 0;
}


/// Symbol of a schematic board by reference ("R1", "U1A") or by index ("#3"); -1 if none.
static int findSymbol( kp_board* b, const char* aRef )
{
    if( aRef[0] == '#' && aRef[1] >= '0' && aRef[1] <= '9' )
        return atoi( aRef + 1 ) < kp_board_footprint_count( b ) ? atoi( aRef + 1 ) : -1;

    for( int i = 0; i < kp_board_footprint_count( b ); ++i )
    {
        if( !strcmp( kp_board_footprint_ref( b, i ), aRef ) || !strcmp( kp_sch_symbol_field_value( b, i, 0 ), aRef ) )
            return i;
    }

    return -1;
}


static bool isNumber( const char* aText )
{
    char* end = nullptr;
    strtod( aText, &end );
    return end && end != aText && *end == 0;
}


/// "REF:NUMBER" of a flat pin (from its description).
static std::string pinName( kp_board* b, int aPin, int aSymbol )
{
    kp_item     it { KP_ITEM_SCH_PIN, aPin, aSymbol, 0, -1 };
    std::string info = kp_board_item_info( b, &it );
    size_t      n = info.find( "\nNumber\t" );
    std::string number = n == std::string::npos ? "?" : info.substr( n + 8, info.find( '\n', n + 8 ) - n - 8 );
    return std::string( kp_sch_symbol_field_value( b, aSymbol, 0 ) ) + ":" + number;
}


/// What sits on a point of the sheet: bit 1 wire, 2 label, 4 junction, 8 note (no-connect,
/// bus entry), 16 sheet pin, 32 a pin of another symbol, 64 another symbol.
static int whatIsAt( kp_board* b, double x, double y, int aSymbol )
{
    kp_item items[32];
    int     n = kp_board_pick( b, x, y, 0.01, nullptr, 0, 0, 0, items, 32 );
    int     mask = 0;

    for( int i = 0; i < n; ++i )
    {
        switch( items[i].type )
        {
        case KP_ITEM_SCH_WIRE: mask |= 1; break;
        case KP_ITEM_SCH_LABEL: mask |= 2; break;
        case KP_ITEM_SCH_JUNCTION: mask |= 4; break;
        case KP_ITEM_SCH_NOTE: mask |= 8; break;
        case KP_ITEM_SCH_SHEET_PIN: mask |= 16; break;
        case KP_ITEM_SCH_PIN: mask |= items[i].footprint != aSymbol ? 32 : 0; break;
        case KP_ITEM_SCH_SYMBOL: mask |= items[i].index != aSymbol ? 64 : 0; break; // e.g. a power symbol
        default: break;
        }
    }

    return mask;
}


/// schedit <in.kicad_sch> <out-dir> [--sheet N] ops...: edit a schematic hierarchy through a
/// kp_sch_doc and write every file of it into out-dir.
static int cmdSchEdit( int argc, char** argv )
{
    namespace fs = std::filesystem;
    const char* inPath = argv[0];
    fs::path    outDir = argv[1];
    int         count = 0;
    char        err[512] = { 0 };
    int         sheet = g_sheet;
    kp_fab_input* in = schematicInput( inPath, count );
    kp_sch_doc*   doc = count ? kp_sch_doc_create( in, 0, err, sizeof( err ) ) : nullptr;
    kp_fab_input_free( in );

    if( !doc )
    {
        fprintf( stderr, "%s: %s\n", inPath, err );
        return 1;
    }

    kp_board* b = kp_board_load_sch_doc( doc, sheet, err, sizeof( err ) );

    if( !b )
    {
        fprintf( stderr, "%s\n", err );
        return 1;
    }

    int failures = 0;

    for( int i = 2; i < argc; )
    {
        std::string op = argv[i++];
        int         ok = 0;
        auto        has = [&]( int n ) { return i + n <= argc; };
        err[0] = 0;

        if( op == "--sheet" && has( 1 ) )
        {
            sheet = atoi( argv[i++] );
            ok = 1;
        }
        else if( op == "move" && has( 3 ) )
        {
            kp_item it { KP_ITEM_SCH_SYMBOL, findSymbol( b, argv[i] ), -1, 0, -1 };
            ok = kp_sch_edit_move( doc, b, &it, 1, atof( argv[i + 1] ), atof( argv[i + 2] ), err, sizeof( err ) );
            printf( "move %s %s %s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "rotate" && has( 1 ) )
        {
            bool    cw = has( 2 ) && !strcmp( argv[i + 1], "cw" );
            kp_item it { KP_ITEM_SCH_SYMBOL, findSymbol( b, argv[i] ), -1, 0, -1 };
            ok = kp_sch_edit_rotate( doc, b, &it, cw ? 0 : 1, err, sizeof( err ) );
            printf( "rotate %s%s: %s %s\n", argv[i], cw ? " cw" : "", ok ? "ok" : "FAILED", err );
            i += cw ? 2 : 1;
        }
        else if( op == "mirror" && has( 2 ) )
        {
            kp_item it { KP_ITEM_SCH_SYMBOL, findSymbol( b, argv[i] ), -1, 0, -1 };
            ok = kp_sch_edit_mirror( doc, b, &it, !strcmp( argv[i + 1], "x" ) ? 1 : 0, err, sizeof( err ) );
            printf( "mirror %s %s: %s %s\n", argv[i], argv[i + 1], ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "delete" && has( 1 ) )
        {
            kp_item it { KP_ITEM_SCH_SYMBOL, findSymbol( b, argv[i] ), -1, 0, -1 };
            ok = kp_sch_edit_delete( doc, b, &it, 1, err, sizeof( err ) );
            printf( "delete %s: %s %s\n", argv[i], ok ? "ok" : "FAILED", err );
            i += 1;
        }
        else if( op == "del" && has( 2 ) )
        {
            // del <wire|junction|label|sheet|sheetpin|note|pin|symbol> <index>
            static const std::pair<const char*, int> kinds[] = {
                { "wire", KP_ITEM_SCH_WIRE },   { "junction", KP_ITEM_SCH_JUNCTION }, { "label", KP_ITEM_SCH_LABEL },
                { "sheet", KP_ITEM_SCH_SHEET }, { "sheetpin", KP_ITEM_SCH_SHEET_PIN }, { "note", KP_ITEM_SCH_NOTE },
                { "pin", KP_ITEM_SCH_PIN },     { "symbol", KP_ITEM_SCH_SYMBOL }
            };
            int type = 0;

            for( const auto& k : kinds )
            {
                if( !strcmp( argv[i], k.first ) )
                    type = k.second;
            }

            kp_item it { type, atoi( argv[i + 1] ), -1, 0, -1 };
            ok = type && kp_sch_edit_delete( doc, b, &it, 1, err, sizeof( err ) );
            printf( "del %s #%s: %s %s\n", argv[i], argv[i + 1], ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "field" && has( 3 ) )
        {
            // VALUE "=" sets the field to the value it has
            int         sym = findSymbol( b, argv[i] );
            std::string value = argv[i + 2];

            if( value == "=" )
            {
                for( int f = 0; f < kp_sch_symbol_field_count( b, sym ); ++f )
                {
                    if( !strcmp( kp_sch_symbol_field_name( b, sym, f ), argv[i + 1] ) )
                        value = kp_sch_symbol_field_value( b, sym, f );
                }
            }

            ok = kp_sch_edit_set_field( doc, b, sym, argv[i + 1], value.c_str(), err, sizeof( err ) );
            printf( "field %s %s=%s: %s %s\n", argv[i], argv[i + 1], value.c_str(), ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "wire" || op == "bus" )
        {
            std::vector<double> xy;

            while( i < argc && isNumber( argv[i] ) )
                xy.push_back( atof( argv[i++] ) );

            ok = kp_sch_edit_add_wire( doc, b, xy.data(), (int32_t) xy.size() / 2, op == "bus", err, sizeof( err ) );
            printf( "%s (%zu points): %s %s\n", op.c_str(), xy.size() / 2, ok ? "ok" : "FAILED", err );
        }
        else if( op == "label" && has( 3 ) )
        {
            // label X Y TEXT [global|hier] [ANGLE]
            double x = atof( argv[i] ), y = atof( argv[i + 1] );
            std::string text = argv[i + 2];
            int    kind = KP_LABEL_LOCAL;
            double angle = 0;
            i += 3;

            if( i < argc && ( !strcmp( argv[i], "global" ) || !strcmp( argv[i], "hier" ) ) )
                kind = !strcmp( argv[i++], "global" ) ? KP_LABEL_GLOBAL : KP_LABEL_HIERARCHICAL;

            if( i < argc && isNumber( argv[i] ) )
                angle = atof( argv[i++] );

            ok = kp_sch_edit_add_label( doc, b, x, y, text.c_str(), kind, angle, err, sizeof( err ) );
            printf( "label %s at %g,%g: %s %s\n", text.c_str(), x, y, ok ? "ok" : "FAILED", err );
        }
        else if( ( op == "nc" || op == "junction" ) && has( 2 ) )
        {
            double x = atof( argv[i] ), y = atof( argv[i + 1] );
            ok = op == "nc" ? kp_sch_edit_add_no_connect( doc, b, x, y, err, sizeof( err ) )
                            : kp_sch_edit_add_junction( doc, b, x, y, err, sizeof( err ) );
            printf( "%s %g,%g: %s %s\n", op.c_str(), x, y, ok ? "ok" : "FAILED", err );
            i += 2;
        }
        else if( op == "dup" && has( 3 ) )
        {
            int sym = findSymbol( b, argv[i] );
            ok = kp_sch_edit_duplicate( doc, b, sym, atof( argv[i + 1] ), atof( argv[i + 2] ), err, sizeof( err ) );
            printf( "dup %s %s %s: %s %s\n", argv[i], argv[i + 1], argv[i + 2], ok ? "ok" : "FAILED", err );
            i += 3;
        }
        else if( op == "snap" && has( 3 ) )
        {
            double x = 0, y = 0;
            int    hit = kp_sch_snap( b, atof( argv[i] ), atof( argv[i + 1] ), atof( argv[i + 2] ), &x, &y );
            printf( "snap %s,%s -> %g,%g%s\n", argv[i], argv[i + 1], x, y, hit ? " (connection)" : "" );
            ok = 1;
            i += 3;
        }
        else if( op == "stroke" && has( 1 ) )
        {
            // stroke RADIUS x0 y0 x1 y1 ...: the wire a pencil stroke gives (no edit)
            double              radius = atof( argv[i++] );
            std::vector<double> xy, out( 64 );

            while( i < argc && isNumber( argv[i] ) )
                xy.push_back( atof( argv[i++] ) );

            int n = kp_sch_wire_from_stroke( b, xy.data(), (int32_t) xy.size() / 2, radius, out.data(), 32 );
            printf( "stroke ->" );

            for( int k = 0; k < n && k < 32; ++k )
                printf( " (%g, %g)", out[2 * k], out[2 * k + 1] );

            printf( "\n" );
            ok = n >= 2;
        }
        else if( op == "movewired" && has( 2 ) )
        {
            // move the first symbol that has a wire on one of its pins
            kp_stats st;
            kp_board_stats( b, &st );
            int sym = -1;

            for( int p = 0; p < st.pads && sym < 0; ++p )
            {
                double  x, y;
                int32_t s = -1;

                if( kp_sch_pin_point( b, p, &x, &y, &s ) && kp_sch_symbol_field_value( b, s, 0 )[0] != '#'
                    && ( whatIsAt( b, x, y, s ) & 1 ) )
                {
                    sym = s;
                }
            }

            kp_item it { KP_ITEM_SCH_SYMBOL, sym, -1, 0, -1 };
            ok = sym >= 0 && kp_sch_edit_move( doc, b, &it, 1, atof( argv[i] ), atof( argv[i + 1] ), err, sizeof( err ) );
            printf( "movewired %s: %s %s %s\n", sym >= 0 ? kp_sch_symbol_field_value( b, sym, 0 ) : "-", argv[i],
                    argv[i + 1], ok ? "ok" : "FAILED" );
            i += 2;
        }
        else if( op == "wirefree" )
        {
            // a wire between the two closest free pins of different symbols
            kp_stats st;
            kp_board_stats( b, &st );
            struct Free { int pin, sym; double x, y; };
            std::vector<Free> free;

            for( int p = 0; p < st.pads && free.size() < 400; ++p )
            {
                double  x, y;
                int32_t s = -1;

                if( kp_sch_pin_point( b, p, &x, &y, &s ) && kp_sch_symbol_field_value( b, s, 0 )[0] != '#'
                    && whatIsAt( b, x, y, s ) == 0 )
                {
                    // pins of the "not connected" type never join anything
                    kp_item it { KP_ITEM_SCH_PIN, p, s, 0, -1 };

                    if( !strstr( kp_board_item_info( b, &it ), "\tNot connected" ) )
                        free.push_back( { p, s, x, y } );
                }
            }

            int    best[2] = { -1, -1 };
            double bestD = 1e300;

            for( size_t a = 0; a < free.size(); ++a )
            {
                for( size_t c = a + 1; c < free.size(); ++c )
                {
                    double d = std::fabs( free[a].x - free[c].x ) + std::fabs( free[a].y - free[c].y );

                    if( free[a].sym != free[c].sym && d > 1.0 && d < bestD )
                    {
                        bestD = d;
                        best[0] = (int) a;
                        best[1] = (int) c;
                    }
                }
            }

            if( best[0] >= 0 )
            {
                const Free& A = free[(size_t) best[0]];
                const Free& B = free[(size_t) best[1]];
                double      xy[] = { A.x, A.y, B.x, A.y, B.x, B.y };
                ok = kp_sch_edit_add_wire( doc, b, xy, 3, 0, err, sizeof( err ) );
                printf( "wirefree %s %s: %s %s\n", pinName( b, A.pin, A.sym ).c_str(), pinName( b, B.pin, B.sym ).c_str(),
                        ok ? "ok" : "FAILED", err );
            }
            else
            {
                printf( "wirefree: no free pins\n" );
            }
        }
        else if( op == "labelwire" && has( 1 ) )
        {
            // a label in the middle of the first horizontal or vertical wire
            ok = 0;

            for( int w = 0; w < 100000 && !ok; ++w )
            {
                kp_item it { KP_ITEM_SCH_WIRE, w, -1, 0, -1 };
                double  x0, y0, x1, y1;

                if( !kp_board_item_bounds( b, &it, 0, &x0, &y0, &x1, &y1 ) )
                    break;

                std::string info = kp_board_item_info( b, &it );

                if( info.rfind( "Wire", 0 ) != 0 )
                    continue;

                double cx = ( x0 + x1 ) / 2, cy = ( y0 + y1 ) / 2;
                bool   horiz = x1 - x0 > 5.0 && y1 - y0 < 1.0, vert = y1 - y0 > 5.0 && x1 - x0 < 1.0;

                if( !horiz && !vert )
                    continue;

                // on the wire's axis, on the grid
                double gx = horiz ? std::round( cx / 1.27 ) * 1.27 : cx;
                double gy = vert ? std::round( cy / 1.27 ) * 1.27 : cy;

                if( std::fabs( gx - std::round( gx / 1.27 ) * 1.27 ) > 1e-3 || std::fabs( gy - std::round( gy / 1.27 ) * 1.27 ) > 1e-3 )
                    continue;

                ok = kp_sch_edit_add_label( doc, b, gx, gy, argv[i], KP_LABEL_LOCAL, vert ? 90 : 0, err, sizeof( err ) );
                printf( "labelwire %s at %g,%g (wire %d): %s %s\n", argv[i], gx, gy, w, ok ? "ok" : "FAILED", err );
            }

            if( !ok )
                printf( "labelwire: no wire\n" );

            i += 1;
        }
        else if( op == "undo" )
        {
            ok = kp_sch_doc_undo( doc );
            printf( "undo: %s\n", ok ? "ok" : "nothing to undo" );
        }
        else if( op == "redo" )
        {
            ok = kp_sch_doc_redo( doc );
            printf( "redo: %s\n", ok ? "ok" : "nothing to redo" );
        }
        else if( op == "save" && has( 1 ) )
        {
            // save SUBDIR: write the files as they are now into out-dir/SUBDIR
            std::error_code ec;

            for( int f = 0; f < kp_sch_doc_file_count( doc ); ++f )
            {
                fs::path    out = outDir / argv[i] / kp_sch_doc_file_name( doc, f );
                size_t      len = 0;
                const char* text = kp_sch_doc_file_text( doc, f, &len );
                fs::create_directories( out.parent_path(), ec );
                std::ofstream o( out, std::ios::binary );
                o.write( text, (std::streamsize) len );
            }

            printf( "save %s\n", argv[i] );
            ok = 1;
            i += 1;
        }
        else
        {
            fprintf( stderr, "bad op %s\n", op.c_str() );
            return 2;
        }

        failures += !ok;

        // rebuild the sheet from the edited document, as the app does
        kp_board* nb = kp_board_load_sch_doc( doc, sheet, err, sizeof( err ) );

        if( !nb )
        {
            fprintf( stderr, "rebuild failed: %s\n", err );
            return 1;
        }

        kp_board_free( b );
        b = nb;
    }

    std::error_code ec;

    for( int f = 0; f < kp_sch_doc_file_count( doc ); ++f )
    {
        fs::path    out = outDir / kp_sch_doc_file_name( doc, f );
        size_t      len = 0;
        const char* text = kp_sch_doc_file_text( doc, f, &len );
        fs::create_directories( out.parent_path(), ec );
        std::ofstream o( out, std::ios::binary );
        o.write( text, (std::streamsize) len );
        printf( "wrote %s (%zu bytes%s)\n", out.string().c_str(), len, kp_sch_doc_file_modified( doc, f ) ? ", modified" : "" );
    }

    printf( "undo: %s\n", kp_sch_doc_can_undo( doc ) ? kp_sch_doc_undo_label( doc ) : "-" );
    kp_board_free( b );
    kp_sch_doc_free( doc );
    return failures ? 3 : 0;
}


/// KPCLI_STRINGS=path/to/Core.strings: use that translation table (like the app does).
static void loadTranslations()
{
    const char* path = getenv( "KPCLI_STRINGS" );
    std::string data;

    if( !path || !readFile( path, data ) )
        return;

    std::vector<std::string> keys, values;
    size_t                   i = 0;

    auto readQuoted = [&]( std::string& out ) -> bool
    {
        i = data.find( '"', i );

        if( i == std::string::npos )
            return false;

        for( ++i; i < data.size() && data[i] != '"'; ++i )
        {
            if( data[i] == '\\' && i + 1 < data.size() )
            {
                ++i;
                out += data[i] == 'n' ? '\n' : data[i];
            }
            else
            {
                out += data[i];
            }
        }

        ++i;
        return true;
    };

    while( true )
    {
        size_t comment = data.find( "/*", i );
        size_t quote = data.find( '"', i );

        if( comment != std::string::npos && comment < quote )
        {
            i = data.find( "*/", comment );
            i = i == std::string::npos ? data.size() : i + 2;
            continue;
        }

        std::string k, v;

        if( !readQuoted( k ) || !readQuoted( v ) )
            break;

        keys.push_back( k );
        values.push_back( v );
    }

    std::vector<const char*> kp, vp;

    for( size_t n = 0; n < keys.size(); ++n )
    {
        kp.push_back( keys[n].c_str() );
        vp.push_back( values[n].c_str() );
    }

    kp_set_translations( kp.data(), vp.data(), (int32_t) kp.size() );
}


static int cmdDrc( int argc, char** argv )
{
    // drc <board> [--pro file.kicad_pro | --no-pro] [--quiet]
    const char* path = argv[0];
    std::string pro;
    bool        quiet = false, noPro = false, all = false;
    std::string proPath;

    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];

        if( a == "--pro" && i + 1 < argc )
            proPath = argv[++i];
        else if( a == "--no-pro" )
            noPro = true;
        else if( a == "--quiet" )
            quiet = true;
        else if( a == "--all" )
            all = true;
    }

    if( proPath.empty() && !noPro )
    {
        std::string p = path;

        if( p.size() > 10 && p.compare( p.size() - 10, 10, ".kicad_pcb" ) == 0 )
            proPath = p.substr( 0, p.size() - 10 ) + ".kicad_pro";
    }

    if( !proPath.empty() && !readFile( proPath.c_str(), pro ) )
        pro.clear();

    kp_board* b = load( path );

    if( !b )
        return 1;

    kp_drc* d = ( all ? kp_board_drc_all : kp_board_drc )( b, pro.empty() ? nullptr : pro.data(), pro.size() );
    int     n = kp_drc_count( d );
    std::map<std::string, int> counts;

    for( int i = 0; i < n; ++i )
    {
        kp_drc_item it;
        kp_drc_get( d, i, &it );
        counts[kp_drc_key( it.code )]++;

        if( !quiet )
            printf( "%-22s %s (%.4f, %.4f) actual=%.4f req=%.4f | %s\n", kp_drc_key( it.code ),
                    it.severity == KP_SEVERITY_ERROR ? "E" : "W", it.x, it.y, it.actual, it.required,
                    kp_drc_message( d, i ) );
    }

    static const char* src[] = { "defaults", "board", "project" };
    printf( "summary rules=%s errors=%d warnings=%d time=%.1fms note='%s'\n", src[kp_drc_rules_source( d )],
            kp_drc_error_count( d ), kp_drc_warning_count( d ), kp_drc_ms( d ), kp_drc_note( d ) );

    for( const auto& [k, c] : counts )
        printf( "count %s %d\n", k.c_str(), c );

    kp_drc_free( d );
    kp_board_free( b );
    return 0;
}


static int cmdFab( int argc, char** argv )
{
    // fab <board> <outdir> [--aux]
    const char* path = argv[0];
    std::string dir = argv[1];
    bool        aux = argc > 2 && !strcmp( argv[2], "--aux" );
    kp_board*   b = load( path );

    if( !b )
        return 1;

    std::string base = path;
    size_t      slash = base.find_last_of( '/' );

    if( slash != std::string::npos )
        base = base.substr( slash + 1 );

    if( base.size() > 10 && base.compare( base.size() - 10, 10, ".kicad_pcb" ) == 0 )
        base.resize( base.size() - 10 );

    kp_fab* f = kp_board_fab( b, base.c_str(), "2026-11-01T12:00:00+03:00", aux ? 1 : 0 );

    for( int i = 0; i < kp_fab_file_count( f ); ++i )
    {
        size_t      len = 0;
        const char* data = kp_fab_file_data( f, i, &len );
        std::string out = dir + "/" + kp_fab_file_name( f, i );
        std::ofstream o( out, std::ios::binary );
        o.write( data, (std::streamsize) len );
        printf( "%-9s %8zu %s\n", kp_fab_file_kind( f, i ), len, kp_fab_file_name( f, i ) );
    }

    size_t      zl = 0;
    const char* z = kp_fab_zip( f, &zl );
    std::ofstream zo( dir + "/" + base + "-fab.zip", std::ios::binary );
    zo.write( z, (std::streamsize) zl );
    printf( "zip       %8zu %s-fab.zip  ok=%d\n", zl, base.c_str(), kp_fab_ok( f ) );

    for( int i = 0; i < kp_fab_warning_count( f ); ++i )
        printf( "warning: %s\n", kp_fab_warning( f, i ) );

    kp_fab_free( f );
    kp_board_free( b );
    return 0;
}


/// Sheets of a schematic: index, page, path of sheet names.
static int cmdSheets( const char* aPath )
{
    kp_board* b = load( aPath );

    if( !b )
        return 1;

    int n = kp_sch_sheet_count( b );

    for( int i = 0; i < n; ++i )
    {
        std::string path;

        for( int s = i; s > 0; )
        {
            kp_sheet_info info;
            kp_sch_sheet_info( b, s, &info );
            path = std::string( "/" ) + kp_sch_sheet_name( b, s ) + path;
            s = info.parent;
        }

        printf( "%d\t%s\t%s\t%s\n", i, kp_sch_sheet_page( b, i ), path.empty() ? "/" : path.c_str(), kp_sch_sheet_file( b, i ) );
    }

    for( int i = 0; i < kp_sch_missing_count( b ); ++i )
        printf( "missing\t%s\n", kp_sch_missing_file( b, i ) );

    kp_board_free( b );
    return 0;
}


int main( int argc, char** argv )
{
    loadTranslations();

    if( argc >= 4 && !strcmp( argv[1], "edit" ) )
        return cmdEdit( argc - 2, argv + 2 );

    if( argc >= 4 && !strcmp( argv[1], "schedit" ) )
        return cmdSchEdit( argc - 2, argv + 2 );

    if( argc >= 4 && !strcmp( argv[1], "fab" ) )
        return cmdFab( argc - 2, argv + 2 );

    if( argc >= 3 && !strcmp( argv[1], "drc" ) )
        return cmdDrc( argc - 2, argv + 2 );

    if( argc >= 3 && !strcmp( argv[1], "sheets" ) )
        return cmdSheets( argv[2] );

    if( argc >= 3 && !strcmp( argv[1], "root" ) )
        return cmdRoot( argv[2] );

    if( argc >= 3 && !strcmp( argv[1], "picksweep" ) )
        return cmdPickSweep( argv[2], argc >= 4 ? atoi( argv[3] ) : 60 );

    if( argc >= 3 && !strcmp( argv[1], "netlist" ) )
    {
        kp_board* b = load( argv[2] );

        if( !b )
            return 1;

        fputs( kp_sch_netlist( b ), stdout );
        kp_board_free( b );
        return 0;
    }

    if( argc >= 3 && !strcmp( argv[1], "stats" ) )
        return cmdStats( argv[2] );

    if( argc >= 4 && !strcmp( argv[1], "render" ) )
        return cmdRender( argc - 2, argv + 2 );

    if( argc >= 5 && !strcmp( argv[1], "pick" ) )
        return cmdPick( argv[2], atof( argv[3] ), atof( argv[4] ), argc >= 6 ? atof( argv[5] ) : 0.2 );

    if( argc >= 3 && !strcmp( argv[1], "check" ) )
        return cmdCheck( argc - 2, argv + 2 );

    fprintf( stderr, "usage:\n  kpcli stats <file>\n  kpcli sheets <file.kicad_sch>\n  kpcli netlist <file.kicad_sch>\n  kpcli check <files...>\n  kpcli pick <file> <x> <y> [tol]\n"
                     "  kpcli render <file> <out.png> [width] [--back] [--view x0 y0 x1 y1] [--pick x y] [--net] [--no-labels]\n"
                     "  kpcli edit <in> <out> ops... (move REF DX DY | rotate REF DEG | delete REF | field REF NAME VALUE |\n"
                     "             width TRACK W | via VIA SIZE DRILL | del KIND INDEX | track X1 Y1 X2 Y2 W LAYER NET |\n"
                     "             addvia X Y SIZE DRILL NET | undo | redo)\n"
                     "  kpcli schedit <in.kicad_sch> <out-dir> ops... (move REF DX DY | rotate REF [cw] | mirror REF x|y |\n"
                     "             delete REF | del KIND INDEX | field REF NAME VALUE|= | wire X1 Y1 X2 Y2... | bus ... |\n"
                     "             label X Y TEXT [global|hier] [ANGLE] | nc X Y | junction X Y | dup REF DX DY |\n"
                     "             snap X Y R | stroke R X0 Y0... | movewired DX DY | wirefree | labelwire TEXT |\n"
                     "             --sheet N | undo | redo)\n" );
    return 2;
}
