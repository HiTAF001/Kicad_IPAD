/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Pencil routing: turns a hand-drawn stroke into a clean track.
 *
 * The stroke is attached to the copper it starts and ends on (pad centre, via, track end or a
 * point along a track), simplified, and rebuilt from horizontal, vertical and 45 degree runs
 * fitted to the hand-drawn line. Sharp corners get 45 degree chamfers. Starting or ending on
 * a track of another copper layer adds a via there. The result is checked against the copper
 * of other nets so the preview can show shorts and clearance problems before committing.
 */
#ifndef NETLOOM_ROUTE_H
#define NETLOOM_ROUTE_H

#include "board.h"
#include "edit.h"
#include "geom.h"

#include <string>
#include <vector>

namespace kp
{

struct RouteRequest
{
    Path   stroke;              ///< raw pencil points, board mm
    int    layer = -1;          ///< copper layer to draw on
    double width = 0.25;        ///< track width
    double tolerance = 0.3;     ///< how far the clean track may stray from the stroke (mm)
    double snapRadius = 0.5;    ///< reach of the stroke ends when looking for copper (mm)
    double viaSize = 0.6;
    double viaDrill = 0.3;
    double clearance = 0.2;     ///< for conflict marking
};

enum class AnchorKind : uint8_t
{
    NONE,
    PAD,
    VIA,
    TRACK_END,
    TRACK_MID
};

struct Anchor
{
    AnchorKind kind = AnchorKind::NONE;
    ItemType   type = ItemType::NONE;
    int        index = -1;
    int        net = 0;
    int        layer = -1;
    Pt         pos;
    bool       needsVia = false; ///< the copper is on another layer: a via joins it
};

struct RouteConflict
{
    Pt   pos;
    int  net = 0;
    bool shorts = false; ///< overlaps (true) or only closer than the clearance (false)
};

struct RouteResult
{
    bool        ok = false;      ///< geometry was produced
    bool        blocked = false; ///< must not be committed (message says why)
    std::string message;
    int         net = 0;
    Anchor      start, end;
    std::vector<NewTrack>      tracks;
    std::vector<NewVia>        vias;
    std::vector<RouteConflict> conflicts;
    bool shorts = false;
    bool tooClose = false;
};

RouteResult RoutePencil( const Board& aBoard, const CopperIndex& aIndex, const RouteRequest& aRequest );

/// Ramer-Douglas-Peucker simplification (indices of kept points, first and last included).
std::vector<size_t> SimplifyPath( const Path& aPts, double aTolerance );

} // namespace kp

#endif
