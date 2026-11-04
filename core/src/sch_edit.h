/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Schematic edits. Each operation works on the file (screen) of one sheet instance, as KiCad
 * does (a file used by several sheets changes for all of them), rewrites only the affected
 * top-level items, patching just the changed nodes inside them, and forms one undo step.
 * New items are written the way the file's KiCad version writes them. Junctions follow
 * KiCad's rule (three or more directions meet) at the points an edit touches.
 */
#ifndef NETLOOM_SCH_EDIT_H
#define NETLOOM_SCH_EDIT_H

#include "mesh.h"
#include "sch_document.h"

#include <string>
#include <vector>

namespace kp
{

/// KiCad's default schematic grid, 50 mil.
constexpr double SCH_GRID = 1.27;

struct SchEditTarget
{
    SchDocument&     doc;
    const Schematic& sch;   ///< built from the document's current state
    int              sheet; ///< index into sch.sheets
};

/// An item of the sheet as picking reports it (SCH_PIN: flat pin index, SCH_NOTE: flat note
/// index, SCH_SHEET_PIN: flat sheet pin index).
struct SchItemRef
{
    ItemType type = ItemType::NONE;
    int      index = -1;
};

enum class SchLabelKind : int { LOCAL = 0, GLOBAL = 1, HIERARCHICAL = 2 };

/// Move items. With aDrag, wire ends on the moving connection points (pins, sheet pins,
/// labels, junctions, ends of moved wires) follow, and a horizontal or vertical wire that
/// would turn slanted gets a bend instead.
bool SchMoveItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, double aDx, double aDy, bool aDrag,
                   std::string& aError );

/// Turn a symbol or label by 90 degrees counter-clockwise about its anchor (KiCad's R).
bool SchRotateItem( SchEditTarget& t, const SchItemRef& aItem, std::string& aError );

/// Mirror a symbol or label: aMirrorX flips top and bottom (KiCad's X), else left and right (Y).
bool SchMirrorItem( SchEditTarget& t, const SchItemRef& aItem, bool aMirrorX, std::string& aError );

bool SchDeleteItems( SchEditTarget& t, const std::vector<SchItemRef>& aItems, std::string& aError );

/// Set an existing field of a symbol. "Reference" sets the reference of the symbol on this
/// sheet (its instance data too).
bool SchSetField( SchEditTarget& t, int aSymbol, const std::string& aName, const std::string& aValue,
                  std::string& aError );

bool SchSetLabelText( SchEditTarget& t, int aLabel, const std::string& aText, std::string& aError );

/// Add a wire (or bus) polyline. Wires the new ends land on are split, and junctions are
/// added where three or more connections meet.
bool SchAddWire( SchEditTarget& t, const std::vector<Pt>& aPoints, bool aBus, std::string& aError );

/// aAngle: 0 = text to the right of the anchor, 90 = up, 180 = left, 270 = down.
bool SchAddLabel( SchEditTarget& t, const Pt& aPos, const std::string& aText, SchLabelKind aKind, int aAngle,
                  std::string& aError );
bool SchAddNoConnect( SchEditTarget& t, const Pt& aPos, std::string& aError );
bool SchAddJunction( SchEditTarget& t, const Pt& aPos, std::string& aError );

/// What an end of a routed wire attaches to.
enum class SchAnchor : int { NONE = 0, PIN = 1, WIRE_END = 2, WIRE = 3, LABEL = 4, JUNCTION = 5, SHEET_PIN = 6,
                             BUS_ENTRY = 7 };

struct SchRoute
{
    std::vector<Pt> points; ///< empty if the stroke gives no wire
    SchAnchor       start = SchAnchor::NONE;
    SchAnchor       end = SchAnchor::NONE;
};

/// A pencil stroke as a wire: the ends snap to the nearest connection point within
/// aSnapRadius (else to the grid), and the path between them is orthogonal, follows the
/// stroke and has at most two bends. No document change.
SchRoute SchRouteStroke( const Schematic& aSch, int aSheet, const std::vector<Pt>& aStroke, double aGrid,
                         double aSnapRadius );

} // namespace kp

#endif
