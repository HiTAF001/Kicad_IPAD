/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Converts a Board into indexed triangle meshes grouped by (layer slot, item kind), ready to be
 * uploaded to the GPU. Within one slot, every pixel must be painted at most once (the renderer
 * uses a per-slot stencil reference) so translucent layers look like KiCad's.
 */
#ifndef KIPAD_MESH_H
#define KIPAD_MESH_H

#include "board.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace kp
{

enum class ItemKind : uint8_t
{
    PAD = 0,
    VIA,
    TRACK,
    GRAPHIC,
    TEXT,
    ZONE,
    WALL_PTH,
    WALL_NPTH,
    WALL_VIA,
    HOLE,
    LABEL0, ///< pad number / net name labels, smallest text bucket (visible only when zoomed in)
    LABEL1,
    LABEL2,
    LABEL3,
    LABEL4, ///< biggest text bucket (visible early)
    COUNT
};

/// Selectable item types (also used by the C API).
enum class ItemType : uint8_t
{
    NONE = 0,
    PAD,
    VIA,
    TRACK,
    ARC,
    FOOTPRINT,
    ZONE,
    SHAPE,
    TEXT,
    COUNT
};

/// A run of triangles (index range in Mesh::indices) belonging to one item on one slot.
struct ItemRange
{
    uint32_t start = 0;
    uint32_t count = 0;
    int32_t  slot = 0;
    uint8_t  kind = 0;
};

struct DrawGroup
{
    int                  slot = 0;
    ItemKind             kind = ItemKind::GRAPHIC;
    std::array<float, 4> color { 1, 1, 1, 1 };
    uint32_t             indexStart = 0;
    uint32_t             indexCount = 0;
    float                minZoom = 0.0f; ///< draw only when zoom (pixels per mm) >= this
};

struct Slot
{
    std::string          name;     ///< canonical name, or "Holes"/"HoleWalls"
    std::string          display;
    int                  layer = -1; ///< board layer index, -1 for virtual slots
    int                  kind = 0;   ///< LayerKind as int, or 10 for virtual
    int                  side = 0;
    int                  sortFront = 0;
    int                  sortBack = 0;
    std::array<float, 4> color { 1, 1, 1, 1 };
    bool                 defaultVisible = true;
    uint32_t             indexCount = 0;
    int                  parentSlot = -1; ///< label slots: only visible when this slot is
    int                  viewSide = 0;    ///< +1 = only in top view, -1 = only in bottom view
};

struct Mesh
{
    std::vector<float>     vertices; ///< x0,y0,x1,y1,... (mm, board frame)
    std::vector<uint32_t>  indices;  ///< triangles
    std::vector<DrawGroup> groups;   ///< in front-view draw order
    std::vector<Slot>      slots;
    BBox                   bounds;   ///< board outline (or content) bounds

    /// Per item triangle ranges, for highlighting: ranges of item i of type t are
    /// ranges[ offsets[t][i] .. offsets[t][i+1] ).
    std::vector<ItemRange>             ranges;
    std::vector<std::vector<uint32_t>> offsets; ///< indexed by ItemType
    std::vector<BBox>                  textBounds; ///< per Board::texts entry
};

/// Background colour used for drilled holes (KiCad default PCB background).
extern const std::array<float, 4> BACKGROUND_COLOR;

void BuildMesh( const Board& aBoard, Mesh& aMesh );

} // namespace kp

#endif
