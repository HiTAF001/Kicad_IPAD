/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Schematic edits. Each operation works on the screen of one sheet (a file used by several
 * sheets is edited once, for all of them, as in KiCad), rewrites only the affected top-level
 * items of the file, patching just the changed nodes, and forms one undo step. New items are
 * written the way the file's KiCad version writes them.
 */
#ifndef NETLOOM_SCH_EDIT_H
#define NETLOOM_SCH_EDIT_H

#include "mesh.h"
#include "sch_document.h"

#include <string>
#include <utility>
#include <vector>

namespace kp
{

/// KiCad's default schematic grid, 50 mil.
constexpr double SCH_GRID = 1.27;

struct SchEditTarget
{
    SchDocument&       doc;
    const SchDocState& state; ///< built from the document's current state
    int                sheet; ///< index into state.sch.sheets
};

/// An item of the sheet as picking reports it (SCH_PIN: flat pin index, SCH_NOTE: flat note
/// index, SCH_SHEET_PIN: flat sheet pin index).
struct SchItemRef
{
    ItemType type = ItemType::NONE;
    int      index = -1;
};

enum class SchLabelKind : int { LOCAL = 0, GLOBAL = 1, HIERARCHICAL = 2 };

/// Move items; wires ending on the pins of moved symbols (and the pins of moved sheets) follow.
bool SchMoveItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, double aDx, double aDy,
                   std::string& aError );
/// Turn a symbol, label or text by 90 degrees about its position.
bool SchRotateItem( SchEditTarget& t, const SchItemRef& aItem, bool aCcw, std::string& aError );
/// Mirror a symbol, label or text: aVertical flips top and bottom (KiCad "mirror x").
bool SchMirrorItem( SchEditTarget& t, const SchItemRef& aItem, bool aVertical, std::string& aError );
bool SchDeleteItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, std::string& aError );
/// Set a field of a symbol (Reference: the reference of the symbol on this sheet).
bool SchSetField( SchEditTarget& t, int aSymbol, const std::string& aName, const std::string& aValue,
                  std::string& aError );
/// Add a wire (or bus) polyline; points snap to connection points or the grid. Wires the new
/// ends land on are split, and junctions are added where three or more connections meet.
bool SchAddWire( SchEditTarget& t, const std::vector<Pt>& aPoints, bool aBus, std::string& aError );
bool SchAddLabel( SchEditTarget& t, const Pt& aPos, const std::string& aText, SchLabelKind aKind, double aAngle,
                  std::string& aError );
bool SchAddNoConnect( SchEditTarget& t, const Pt& aPos, std::string& aError );
bool SchAddJunction( SchEditTarget& t, const Pt& aPos, std::string& aError );
/// Copy a symbol with new UUIDs and the next free reference of its prefix.
bool SchDuplicateSymbol( SchEditTarget& t, int aSymbol, double aDx, double aDy, std::string& aError );

/// Nearest connection point (pin tip, wire end, label anchor, sheet pin, junction, bus entry
/// end) within aRadius; else the nearest grid point. True if a connection point was hit.
bool SchSnapPoint( const Schematic& aSch, int aSheet, const Pt& aPoint, double aRadius, Pt& aOut );

/// A pencil stroke as an orthogonal polyline (a few bends at most) between snapped ends.
std::vector<Pt> SchWireFromStroke( const Schematic& aSch, int aSheet, const std::vector<Pt>& aStroke,
                                   double aRadius );

/// Fields of a symbol as shown on a sheet: name, value (Reference first, without unit letter).
std::vector<std::pair<std::string, std::string>> SchSymbolFields( const Schematic& aSch, int aSheet, int aSymbol );

} // namespace kp

#endif
