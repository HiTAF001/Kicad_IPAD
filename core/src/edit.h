/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Board edits. Each operation rewrites only the affected top-level items of the document,
 * patching just the changed nodes inside them, and forms one undo step.
 */
#ifndef NETLOOM_EDIT_H
#define NETLOOM_EDIT_H

#include "board.h"
#include "document.h"
#include "mesh.h"

#include <string>
#include <vector>

namespace kp
{

struct EditTarget
{
    Document&               doc;
    const Board&            board;
    const std::vector<int>& docMap; ///< board parse index -> document item index
};

bool MoveFootprint( EditTarget& t, int aFootprint, double aDx, double aDy, std::string& aError );
bool RotateFootprint( EditTarget& t, int aFootprint, double aDegrees, std::string& aError );
bool DeleteItem( EditTarget& t, ItemType aType, int aIndex, std::string& aError );
/// Several fields in one undo step (e.g. reference and value).
bool SetFootprintFields( EditTarget& t, int aFp, const std::vector<std::pair<std::string, std::string>>& aFields,
                         std::string& err );
bool SetFootprintField( EditTarget& t, int aFootprint, const std::string& aField, const std::string& aValue,
                        std::string& aError );
bool SetTrackWidth( EditTarget& t, ItemType aType, int aIndex, double aWidth, std::string& aError );
bool SetViaSize( EditTarget& t, int aVia, double aSize, double aDrill, std::string& aError );

struct NewTrack
{
    Pt     start, end;
    double width = 0.25;
    int    layer = -1;
    int    net = 0;
};

struct NewVia
{
    Pt     pos;
    double size = 0.6;
    double drill = 0.3;
    int    layerA = -1; ///< -1 = outermost copper layers (through via)
    int    layerB = -1;
    int    net = 0;
};

/// Add tracks and vias in one undo step, written the way the file's KiCad version writes them.
bool AddItems( EditTarget& t, const std::vector<NewTrack>& aTracks, const std::vector<NewVia>& aVias,
               const std::string& aLabel, std::string& aError );

/// Serialized forms (exposed for tests).
std::string TrackText( const Board& aBoard, const NewTrack& aTrack );
std::string ViaText( const Board& aBoard, const NewVia& aVia );

/// KiCad style number formatting (up to 6 decimals, no trailing zeros).
std::string FormatNumber( double aValue );

/// KiCad style quoted string.
std::string QuoteString( const std::string& aText );

} // namespace kp

#endif
