/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Exact copper geometry for routing and design rule checks.
 *
 * Every copper item is described as a union of primitives: a point, a segment or a simple
 * polygon, each grown by a radius. That covers round, oval, rectangular and rounded pads,
 * tracks, arcs (as short segments), vias and polygons exactly, and makes the distance between
 * two items a few segment-to-segment distances.
 */
#ifndef NETLOOM_GEOM_H
#define NETLOOM_GEOM_H

#include "board.h"
#include "mesh.h"

#include <memory>
#include <unordered_set>
#include <vector>

namespace kp
{

struct Prim
{
    enum Kind : uint8_t
    {
        POINT,
        SEGMENT,
        POLYGON, ///< closed, filled
        CHAIN    ///< open polyline (a piece of a zone fill outline)
    };

    Kind   kind = POINT;
    Pt     a, b;     ///< POINT: a; SEGMENT: a-b
    Path   poly;     ///< POLYGON outline (implicitly closed) or CHAIN points
    double r = 0.0;  ///< growth radius
    BBox   box;      ///< including r

    void Finish();
};

/// Fast point-in-polygon for big polygons (zone fills): edges bucketed by rows.
struct AreaIndex
{
    const Path*                        poly = nullptr;
    BBox                               box;
    double                             y0 = 0, rowH = 1;
    int                                rows = 0;
    std::vector<std::vector<uint32_t>> buckets;

    void Build( const Path& aPoly );
    bool Contains( const Pt& aPoint ) const;
};

/// Grid over the primitives of a shape with many of them (zone fills, complex graphics).
struct PrimGrid
{
    double                        x0 = 0, y0 = 0, cell = 1;
    int                           nx = 0, ny = 0;
    std::vector<std::vector<int>> cells;

    void Build( const std::vector<struct Prim>& aPrims, const BBox& aBox );
    /// Primitive indices whose box may be within aBox (sorted, unique).
    void Query( const BBox& aBox, std::vector<int>& aOut ) const;
};

struct CuShape
{
    ItemType          type = ItemType::NONE;
    int               index = -1;
    int               net = 0;
    int               layer = -1;
    int               footprint = -1;
    std::vector<Prim> prims;
    std::vector<const AreaIndex*> areas; ///< filled regions given by their outline only (zone fills)
    std::shared_ptr<PrimGrid>     grid;  ///< for shapes with many primitives
    BBox              box;

    void Finish();
};

double SegSegDistance( const Pt& a1, const Pt& a2, const Pt& b1, const Pt& b2 );

/// Gap between the surfaces of two primitives (0 when they touch or overlap).
double PrimDistance( const Prim& aA, const Prim& aB );

/// Gap between two shapes (0 when they touch or overlap).
double ShapeDistance( const CuShape& aA, const CuShape& aB );

/// Same, but stops early: returns aCutoff when the gap is at least aCutoff (aCutoff <= 0: exact).
double ShapeDistanceTo( const CuShape& aA, const CuShape& aB, double aCutoff );

/// Closest points of two primitives' cores (for marker placement).
Pt PrimNearPoint( const Prim& aA, const Prim& aB );

/// Pad geometry used on a layer (padstacks may differ per layer).
const PadGeom& PadGeomOnLayer( const Board& aBoard, const Pad& aPad, int aLayer );

/// Primitives of a pad's copper outline, grown by aGrow (board frame).
void PadPrims( const Pad& aPad, const PadGeom& aGeom, double aGrow, std::vector<Prim>& aOut,
               double aArcError = 0.002 );

/// Is the via present on this copper layer, and its diameter there.
bool ViaOnLayer( const Board& aBoard, const Via& aVia, int aLayer, double* aSize = nullptr );

/// Primitives of a graphic shape's copper (stroke and fill).
void ShapePrims( const Shape& aShape, std::vector<Prim>& aOut, double aArcError = 0.002 );

/// All copper items, one CuShape per (item, copper layer), with a per-layer grid index.
class CopperIndex
{
public:
    /// aArcError: max deviation of arcs flattened to segments (mm).
    /// aFlashing: pads and vias set to drop unused layers keep only their hole where nothing of
    /// their net connects on a layer (as KiCad).
    void Build( const Board& aBoard, bool aZones = false, bool aGraphics = true, double aArcError = 0.002,
                bool aFlashing = false );

    const std::vector<CuShape>& Shapes() const { return m_shapes; }

    /// Shape ids on aLayer whose box overlaps aBox (each id once, unordered).
    void Query( int aLayer, const BBox& aBox, std::vector<int>& aOut ) const;

    /// After Build( ..., aFlashing = true ): does this pad/via keep only its hole on aLayer?
    bool HoleOnly( ItemType aType, int aIndex, int aLayer ) const
    {
        return m_holeOnly.count( flashKey( aType, aIndex, aLayer ) ) > 0;
    }

    /// Add a temporary shape (not indexed); returns its id.
    int AddUnindexed( const CuShape& aShape );

private:
    void applyFlashing( const Board& aBoard );

    static uint64_t flashKey( ItemType t, int i, int l )
    {
        return ( (uint64_t) t << 56 ) | ( (uint64_t) (uint32_t) i << 16 ) | (uint64_t) ( l & 0xFFFF );
    }

    std::unordered_set<uint64_t> m_holeOnly;

    struct Grid
    {
        double                        x0 = 0, y0 = 0, cell = 1;
        int                           nx = 0, ny = 0;
        std::vector<std::vector<int>> cells;
        std::vector<int>              big; ///< shapes spanning many cells
    };

    std::vector<CuShape>      m_shapes;
    std::vector<std::unique_ptr<AreaIndex>> m_areas;
    std::vector<Grid>         m_grids; ///< by board layer
    mutable std::vector<int>  m_stamp;
    mutable int               m_stampValue = 0;
};

} // namespace kp

#endif
