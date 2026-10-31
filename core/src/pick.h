/*
 * KiPad - KiCad board viewer for iPad
 * Copyright (C) 2026 KiPad contributors
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Picking (what is under the finger), item descriptions and highlight ranges.
 */
#ifndef KIPAD_PICK_H
#define KIPAD_PICK_H

#include "board.h"
#include "mesh.h"

#include <string>
#include <vector>

namespace kp
{

struct PickHit
{
    ItemType type = ItemType::NONE;
    int      index = -1;
    int      footprint = -1;
    int      net = 0;
    int      layer = -1;
    int      priority = 99;   ///< lower = preferred
    int      layerOrder = 0;  ///< higher = closer to the viewer
    double   size = 0.0;      ///< smaller items first among equals
};

struct PickFilter
{
    const uint8_t* slotVisible = nullptr; ///< per mesh slot, may be null (= all visible)
    int            slotCount = 0;
    uint32_t       hiddenKinds = 0;       ///< bit per ItemKind
    bool           flipped = false;

    bool layerVisible( int aLayer ) const
    {
        if( aLayer < 0 )
            return false;

        if( !slotVisible || aLayer >= slotCount )
            return true;

        return slotVisible[aLayer] != 0;
    }

    bool kindVisible( ItemKind k ) const { return ( hiddenKinds & ( 1u << (unsigned) k ) ) == 0; }
};

/// Items under aPoint (mm), best candidate first.
void Pick( const Board& aBoard, const Mesh& aMesh, const Pt& aPoint, double aTolerance,
           const PickFilter& aFilter, std::vector<PickHit>& aOut );

/// Centre of the nearest visible pad or via within aTolerance (for the measuring tool).
bool Snap( const Board& aBoard, const Pt& aPoint, double aTolerance, const PickFilter& aFilter,
           Pt& aOut );

/// "Title\nKey\tValue\n..." description of an item (UTF-8, Turkish).
std::string ItemInfo( const Board& aBoard, const Mesh& aMesh, ItemType aType, int aIndex );

/// Description of a net.
std::string NetInfo( const Board& aBoard, int aNet );

BBox ItemBounds( const Board& aBoard, const Mesh& aMesh, ItemType aType, int aIndex );
BBox NetBounds( const Board& aBoard, int aNet );

/// Triangle ranges to highlight: one item (a footprint means all its children) or a whole net.
void HighlightRanges( const Board& aBoard, const Mesh& aMesh, ItemType aType, int aIndex,
                      bool aWholeNet, int aNet, std::vector<ItemRange>& aOut );

} // namespace kp

#endif
