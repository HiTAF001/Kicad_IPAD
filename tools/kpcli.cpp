/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
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
 *   kpcli stats  <board>
 *   kpcli check  <board>...                 (exit code != 0 if any board fails to load)
 *   kpcli pick   <board> <x> <y> [tol]      (items under a point, with their descriptions)
 *   kpcli render <board> <out.png> [width] [options]
 *        --back                 view from the bottom side
 *        --view x0 y0 x1 y1     render only this window (mm)
 *        --pick x y             select the item at (x, y) and highlight it
 *        --net                  with --pick: highlight the item's whole net
 *        --no-labels            hide pad/net labels
 */
#include "kpcore.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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


static kp_board* load( const char* aPath )
{
    std::string data;

    if( !readFile( aPath, data ) )
    {
        fprintf( stderr, "cannot read %s\n", aPath );
        return nullptr;
    }

    char      err[512] = { 0 };
    kp_board* b = kp_board_load( data.data(), data.size(), err, sizeof( err ) );

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
    printf( "time: parse %.1f ms, mesh %.1f ms\n", s.parse_ms, s.mesh_ms );

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
    static const char* names[] = { "none", "pad", "via", "track", "arc", "footprint", "zone", "shape", "text" };
    printf( "-- %s #%d (net %d, fp %d, layer %d)\n", names[it.type], it.index, it.net, it.footprint, it.layer );
    printf( "%s\n", kp_board_item_info( b, &it ) );
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

    for( size_t i = 0; i < (size_t) R.w * R.h; ++i )
    {
        R.color[i * 3 + 0] = 0.0f;
        R.color[i * 3 + 1] = 16 / 255.0f;
        R.color[i * 3 + 2] = 35 / 255.0f;
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

        printf( "OK  v%-8d fp=%-5d pads=%-6d trk=%-6d via=%-5d zone=%-3d tri=%-8lld picks=%-3d warn=%d  %.0f+%.0f ms  %s\n",
                s.file_version, s.footprints, s.pads, s.tracks + s.arcs, s.vias, s.zones,
                (long long) s.triangle_count, picks, s.warning_count, s.parse_ms, s.mesh_ms, argv[i] );
        (void) ranges;
        kp_board_free( b );
    }

    printf( "%d/%d loaded\n", argc - failed, argc );
    return failed ? 1 : 0;
}


int main( int argc, char** argv )
{
    if( argc >= 3 && !strcmp( argv[1], "stats" ) )
        return cmdStats( argv[2] );

    if( argc >= 4 && !strcmp( argv[1], "render" ) )
        return cmdRender( argc - 2, argv + 2 );

    if( argc >= 5 && !strcmp( argv[1], "pick" ) )
        return cmdPick( argv[2], atof( argv[3] ), atof( argv[4] ), argc >= 6 ? atof( argv[5] ) : 0.2 );

    if( argc >= 3 && !strcmp( argv[1], "check" ) )
        return cmdCheck( argc - 2, argv + 2 );

    fprintf( stderr, "usage:\n  kpcli stats <file>\n  kpcli check <files...>\n  kpcli pick <file> <x> <y> [tol]\n"
                     "  kpcli render <file> <out.png> [width] [--back] [--view x0 y0 x1 y1] [--pick x y] [--net] [--no-labels]\n" );
    return 2;
}
