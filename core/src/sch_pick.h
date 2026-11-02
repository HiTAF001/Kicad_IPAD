/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Schematic sheets: picking items by their drawn triangles, item and net descriptions,
 * highlight ranges and bounds. Net indices here are indices into Schematic::nets (-1 = none).
 */
#ifndef NETLOOM_SCH_PICK_H
#define NETLOOM_SCH_PICK_H

#include "pick.h"
#include "schematic.h"

#include <string>
#include <vector>

namespace kp
{

/// Bounding boxes of the drawn items of one sheet mesh (filled on first use).
struct SchPickCache
{
    std::vector<std::vector<BBox>> boxes; ///< per ItemType, per item
};

/// Items of sheet aSheet under aPoint, best first. PickHit::net is a Schematic::nets index or -1;
/// PickHit::footprint is the symbol of a pin (or the sheet of a sheet pin), else -1.
void SchPick( const Schematic& aSch, int aSheet, const Mesh& aMesh, SchPickCache& aCache, const Pt& aPoint,
              double aTolerance, const PickFilter& aFilter, std::vector<PickHit>& aOut );

/// Net of an item of the sheet, -1 if none.
int SchItemNet( const Schematic& aSch, int aSheet, ItemType aType, int aIndex );

/// "Title\nKey\tValue..." descriptions.
std::string SchItemInfo( const Schematic& aSch, int aSheet, ItemType aType, int aIndex );
std::string SchNetInfo( const Schematic& aSch, int aSheet, int aNet );

BBox SchItemBounds( const Schematic& aSch, int aSheet, const Mesh& aMesh, ItemType aType, int aIndex );
BBox SchNetBounds( const Schematic& aSch, int aSheet, const Mesh& aMesh, int aNet );

/// Ranges of one item (a symbol with its pins, a sheet with its pins) or of every item of a net
/// on this sheet.
void SchHighlightRanges( const Schematic& aSch, int aSheet, const Mesh& aMesh, ItemType aType, int aIndex,
                         bool aWholeNet, int aNet, std::vector<ItemRange>& aOut );

/// Sheets (indices into Schematic::sheets) on which a net has items.
std::vector<int> SchNetSheets( const Schematic& aSch, int aNet );

} // namespace kp

#endif
