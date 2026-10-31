/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Lightweight board model. All coordinates are in millimetres, board frame, Y axis pointing
 * down (same as KiCad). Angles are degrees, KiCad convention (positive = counter-clockwise on
 * screen).
 */
#ifndef KIPAD_BOARD_H
#define KIPAD_BOARD_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace kp
{

struct Pt
{
    double x = 0.0;
    double y = 0.0;

    Pt() = default;
    Pt( double aX, double aY ) :
            x( aX ),
            y( aY )
    {
    }

    Pt operator+( const Pt& o ) const { return { x + o.x, y + o.y }; }
    Pt operator-( const Pt& o ) const { return { x - o.x, y - o.y }; }
    Pt operator*( double s ) const { return { x * s, y * s }; }
};

using Path = std::vector<Pt>;

struct BBox
{
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;

    bool valid() const { return minX <= maxX && minY <= maxY; }

    void add( const Pt& p )
    {
        if( p.x < minX ) minX = p.x;
        if( p.y < minY ) minY = p.y;
        if( p.x > maxX ) maxX = p.x;
        if( p.y > maxY ) maxY = p.y;
    }

    void add( const Pt& p, double r )
    {
        add( Pt( p.x - r, p.y - r ) );
        add( Pt( p.x + r, p.y + r ) );
    }

    void add( const BBox& b )
    {
        if( b.valid() )
        {
            add( Pt( b.minX, b.minY ) );
            add( Pt( b.maxX, b.maxY ) );
        }
    }
};

/// KiCad rotation: positive angle = counter-clockwise on a Y-down screen.
Pt RotatePt( const Pt& p, double aDeg );
Pt RotatePt( const Pt& p, const Pt& aCenter, double aDeg );

enum class LayerKind : uint8_t
{
    COPPER,
    SILK,
    MASK,
    PASTE,
    ADHES,
    FAB,
    COURTYARD,
    EDGE,
    MARGIN,
    USER
};

struct Layer
{
    std::string canonical;  ///< e.g. "F.Cu"
    std::string display;    ///< user name if any, e.g. "top_layer"
    LayerKind   kind = LayerKind::USER;
    int         side = 0;          ///< -1 = back, +1 = front, 0 = neither / inner
    int         copperOrder = -1;  ///< 0 = F.Cu, 1..n = inner, last = B.Cu (copper only)
    int         sortKey = 0;       ///< draw order when viewed from the front (low = first)
    std::array<float, 4> color { 0.8f, 0.8f, 0.8f, 1.0f };
    bool        defaultVisible = true;
};

enum class ShapeType : uint8_t
{
    SEGMENT,
    ARC,
    CIRCLE,
    RECT,
    POLY,
    BEZIER
};

struct Shape
{
    ShapeType type = ShapeType::SEGMENT;
    Pt        start, mid, end; ///< SEGMENT/RECT: start,end; ARC: start,mid,end; CIRCLE: start=center, end=point
    Pt        c1, c2;          ///< bezier control points
    Path      poly;            ///< POLY outline (arcs already flattened)
    double    width = 0.0;     ///< stroke width (0 = no stroke)
    double    radius = 0.0;    ///< rectangle corner radius
    bool      filled = false;
    int       layer = -1;
    int       footprint = -1;
};

enum class HAlign : int8_t { LEFT = -1, CENTER = 0, RIGHT = 1 };
enum class VAlign : int8_t { TOP = -1, CENTER = 0, BOTTOM = 1 };

struct Text
{
    std::string text;        ///< raw text (variables unresolved)
    Pt          pos;
    double      angle = 0.0; ///< draw rotation, degrees
    double      sizeX = 1.0; ///< glyph width
    double      sizeY = 1.0; ///< glyph height
    double      thickness = 0.0;
    double      lineSpacing = 1.0;
    bool        bold = false;
    bool        italic = false;
    bool        mirror = false;
    bool        visible = true;
    bool        keepUpright = false;
    bool        knockout = false;
    HAlign      hAlign = HAlign::CENTER;
    VAlign      vAlign = VAlign::CENTER;
    int         layer = -1;
    int         footprint = -1;
    bool        hasFace = false;
    /// Outline-font glyph polygons cached by KiCad (absolute coords); each entry:
    /// [0] = outline, [1..] = holes.
    std::vector<std::vector<Path>> renderCache;
};

enum class PadAttr : uint8_t { PTH, SMD, CONN, NPTH };
enum class PadShape : uint8_t { CIRCLE, RECT, OVAL, TRAPEZOID, ROUNDRECT, CHAMFERED_RECT, CUSTOM };

enum ChamferCorner : uint8_t
{
    CHAMFER_TOP_LEFT = 1,
    CHAMFER_TOP_RIGHT = 2,
    CHAMFER_BOTTOM_LEFT = 4,
    CHAMFER_BOTTOM_RIGHT = 8
};

struct PadGeom
{
    PadShape shape = PadShape::CIRCLE;
    Pt       size { 1.0, 1.0 };
    Pt       offset;              ///< shape offset from hole, pad-local frame
    Pt       delta;               ///< trapezoid rect_delta
    double   rratio = 0.25;
    double   chamferRatio = 0.0;
    uint8_t  chamferCorners = 0;
    PadShape anchor = PadShape::CIRCLE; ///< custom pad anchor shape (CIRCLE or RECT)
    std::vector<Shape> primitives;      ///< custom pad primitives, pad-local frame
};

struct Pad
{
    std::string number;
    PadAttr     attr = PadAttr::SMD;
    Pt          pos;          ///< board frame
    double      angle = 0.0;  ///< board frame orientation
    PadGeom     geom;         ///< default (F.Cu / all layers) geometry
    std::vector<std::pair<int, PadGeom>> layerGeom; ///< per-layer overrides (padstacks)
    int         innerGeom = -1; ///< index in layerGeom used for all inner layers (front_inner_back)
    std::vector<int> layers;
    Pt          drill;        ///< drill size (x,y); 0 = no hole
    bool        drillOval = false;
    int         net = 0;
    int         footprint = -1;
    std::string pinFunction;
    std::string pinType;
};

struct Track
{
    Pt     start, end;
    double width = 0.2;
    int    layer = -1;
    int    net = 0;
};

struct ArcTrack
{
    Pt     start, mid, end;
    double width = 0.2;
    int    layer = -1;
    int    net = 0;
};

enum class ViaType : uint8_t { THROUGH, BLIND, BURIED, MICRO };

struct Via
{
    Pt      pos;
    double  size = 0.6;
    double  drill = 0.3;
    int     layerA = -1;
    int     layerB = -1;
    ViaType type = ViaType::THROUGH;
    std::vector<std::pair<int, double>> sizeOverride; ///< per-layer size (padstack)
    double  innerSize = 0.0;
    int     net = 0;
};

struct ZoneFill
{
    int               layer = -1;
    std::vector<Path> polys; ///< each filled_polygon (KiCad stores them "fractured", no holes)
};

struct Zone
{
    int                   net = 0;
    std::string           netName;
    std::vector<int>      layers;
    bool                  keepout = false;
    std::string           name;
    Path                  outline;
    std::vector<ZoneFill> fills;
    int                   footprint = -1;
    double                minThickness = 0.0;
    bool                  strokeFill = false; ///< legacy zones: fill outlines drawn with min width
};

struct Footprint
{
    std::string libId;
    std::string reference;
    std::string value;
    Pt          pos;
    double      angle = 0.0;
    bool        back = false;
    int         layer = -1;
    int         padCount = 0;
    std::unordered_map<std::string, std::string> fields;
    std::vector<std::string> attrs;
    BBox        localBox; ///< outline in the footprint frame (rotate by -angle around pos)
};

struct BoardStats
{
    int    footprints = 0;
    int    pads = 0;
    int    tracks = 0;
    int    arcs = 0;
    int    vias = 0;
    int    zones = 0;
    int    keepouts = 0;
    int    nets = 0;
    int    copperLayers = 0;
    int    graphics = 0;
    int    texts = 0;
    int    holes = 0;
    double widthMM = 0.0;
    double heightMM = 0.0;
};

class Board
{
public:
    int         version = 0;
    std::string generator;
    std::string generatorVersion;
    std::map<std::string, std::string> titleBlock;
    double      thickness = 1.6;

    std::vector<Layer>     layers;
    std::vector<std::string> nets; ///< index = internal net id, 0 = no net

    std::vector<Footprint> footprints;
    std::vector<Pad>       pads;
    std::vector<Track>     tracks;
    std::vector<ArcTrack>  arcs;
    std::vector<Via>       vias;
    std::vector<Zone>      zones;
    std::vector<Shape>     shapes;
    std::vector<Text>      texts;

    std::vector<std::string> warnings;

    /// Copper layer indices in stack order (F.Cu first, B.Cu last).
    std::vector<int> copperStack;

    int  LayerIndex( const std::string& aName ) const;
    BBox OutlineBBox() const;    ///< bounding box of Edge.Cuts (invalid if none)
    BBox ContentBBox() const;    ///< bounding box of all geometry
    BoardStats Stats() const;

    /// Compute derived data after parsing (footprint boxes).
    void Finalize();

    /// Resolve ${VAR} references in a text owned by footprint aFootprint (or -1).
    std::string ResolveText( const std::string& aText, int aFootprint, int aLayer ) const;

    // lookup tables used by the parser
    std::unordered_map<std::string, int> layerByName;
    std::unordered_map<int, int>         netByNumber;
    std::unordered_map<std::string, int> netByName;
};

/// Parse a .kicad_pcb (KiCad 4 .. 11) from memory. Throws kp::ParseError on fatal errors.
void LoadBoard( Board& aBoard, const char* aData, size_t aLen );

/// Approximate an arc through three points as a polyline (including both ends).
void ArcToPoints( Path& aOut, const Pt& aStart, const Pt& aMid, const Pt& aEnd, double aMaxErr );

/// Approximate an arc given center, start point and sweep (degrees, KiCad sense).
void ArcToPointsCenter( Path& aOut, const Pt& aCenter, const Pt& aStart, double aSweepDeg,
                        double aMaxErr );

/// Number of segments for a full circle of radius aRadius with max error aMaxErr.
int CircleSegments( double aRadius, double aMaxErr );

} // namespace kp

#endif
