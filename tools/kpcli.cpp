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
 * stencil, straight alpha blending), so the output PNG shows what the app will draw.
 *
 *   kpcli stats  <board.kicad_pcb>
 *   kpcli render <board.kicad_pcb> <out.png> [width] [--back]
 *   kpcli check  <board.kicad_pcb>...      (exit code != 0 if any board fails to load)
 */
#include "kpcore.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
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
        printf( "  %2d %-10s %-16s kind=%d side=%+d front=%d back=%d idx=%u\n", i,
                kp_board_slot_name( b, i ), kp_board_slot_display( b, i ), si.kind, si.side,
                si.sort_front, si.sort_back, si.index_count );
    }

    kp_board_free( b );
    return 0;
}


static int cmdRender( const char* aPath, const char* aOut, int aWidth, bool aBack )
{
    kp_board* b = load( aPath );

    if( !b )
        return 1;

    double x0, y0, x1, y1;
    kp_board_bounds( b, &x0, &y0, &x1, &y1 );
    double mw = ( x1 - x0 ) * 0.03 + 0.5, mh = ( y1 - y0 ) * 0.03 + 0.5;
    x0 -= mw;
    x1 += mw;
    y0 -= mh;
    y1 += mh;

    const int SS = 2;
    int       W = aWidth;
    int       H = std::max( 16, (int) std::lround( aWidth * ( y1 - y0 ) / ( x1 - x0 ) ) );
    int       w = W * SS, h = H * SS;
    double    scale = w / ( x1 - x0 );

    std::vector<float>   color( (size_t) w * h * 3 );
    std::vector<uint8_t> stencil( (size_t) w * h, 0 );

    for( size_t i = 0; i < (size_t) w * h; ++i )
    {
        color[i * 3 + 0] = 0.0f;
        color[i * 3 + 1] = 16 / 255.0f;
        color[i * 3 + 2] = 35 / 255.0f;
    }

    size_t          nv = 0, ni = 0;
    const float*    V = kp_board_vertices( b, &nv );
    const uint32_t* I = kp_board_indices( b, &ni );

    int nslots = kp_board_slot_count( b );
    std::vector<kp_slot_info> slots( (size_t) nslots );

    for( int i = 0; i < nslots; ++i )
        kp_board_slot_info( b, i, &slots[(size_t) i] );

    int ng = kp_board_group_count( b );
    std::vector<kp_group> groups( (size_t) ng );

    for( int i = 0; i < ng; ++i )
        kp_board_group( b, i, &groups[(size_t) i] );

    std::stable_sort( groups.begin(), groups.end(),
                      [&]( const kp_group& a, const kp_group& c )
                      {
                          const kp_slot_info& sa = slots[(size_t) a.slot];
                          const kp_slot_info& sc = slots[(size_t) c.slot];
                          return ( aBack ? sa.sort_back : sa.sort_front )
                                 < ( aBack ? sc.sort_back : sc.sort_front );
                      } );

    auto toPx = [&]( uint32_t idx, double& px, double& py )
    {
        double x = V[idx * 2], y = V[idx * 2 + 1];

        if( aBack )
            x = x1 - ( x - x0 );

        px = ( x - x0 ) * scale;
        py = ( y - y0 ) * scale;
    };

    for( const kp_group& g : groups )
    {
        uint8_t ref = (uint8_t) ( g.slot % 255 + 1 );
        float   a = g.a;

        for( uint32_t t = g.index_start; t + 2 < g.index_start + g.index_count; t += 3 )
        {
            double ax, ay, bx, by, cx, cy;
            toPx( I[t], ax, ay );
            toPx( I[t + 1], bx, by );
            toPx( I[t + 2], cx, cy );

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
                    c[0] = g.r * a + c[0] * ( 1 - a );
                    c[1] = g.g * a + c[1] * ( 1 - a );
                    c[2] = g.b * a + c[2] * ( 1 - a );
                }
            }
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
                        sum += color[( (size_t) ( y * SS + sy ) * w + ( x * SS + sx ) ) * 3 + ch];

                img[( (size_t) y * W + x ) * 3 + ch] =
                        (uint8_t) std::clamp( (int) std::lround( sum / ( SS * SS ) * 255 ), 0, 255 );
            }
        }
    }

    int ok = stbi_write_png( aOut, W, H, 3, img.data(), W * 3 );
    printf( "%s: %dx%d, %d groups, %zu triangles -> %s\n", aPath, W, H, ng, ni / 3,
            ok ? aOut : "WRITE FAILED" );
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
        printf( "OK  v%-8d fp=%-5d pads=%-6d trk=%-6d via=%-5d zone=%-3d tri=%-8lld warn=%d  %.0f+%.0f ms  %s\n",
                s.file_version, s.footprints, s.pads, s.tracks + s.arcs, s.vias, s.zones,
                (long long) s.triangle_count, s.warning_count, s.parse_ms, s.mesh_ms, argv[i] );
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
    {
        int  width = 1600;
        bool back = false;

        for( int i = 4; i < argc; ++i )
        {
            if( !strcmp( argv[i], "--back" ) )
                back = true;
            else
                width = atoi( argv[i] );
        }

        return cmdRender( argv[2], argv[3], width, back );
    }

    if( argc >= 3 && !strcmp( argv[1], "check" ) )
        return cmdCheck( argc - 2, argv + 2 );

    fprintf( stderr, "usage:\n  kpcli stats <file>\n  kpcli render <file> <out.png> [width] [--back]\n"
                     "  kpcli check <files...>\n" );
    return 2;
}
