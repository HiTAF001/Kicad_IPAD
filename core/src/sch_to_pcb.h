/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * "Update PCB from Schematic". The schematic's symbols and nets are compared with a board the
 * way KiCad's BOARD_NETLIST_UPDATER does it with its default options, which gives a plan: what
 * would change (references, values, symbol associations, pad nets, nets) and what is only
 * reported (footprints that are missing on the board or have no symbol, other footprint library
 * ids). The plan is applied to the board's Document in one undo step; only the touched pads,
 * footprints and net declarations are rewritten, in the file's own format.
 */
#ifndef NETLOOM_SCH_TO_PCB_H
#define NETLOOM_SCH_TO_PCB_H

#include "board.h"
#include "edit.h"
#include "schematic.h"

#include <string>
#include <vector>

namespace kp
{

/// Kind of a line of the plan (the values are part of the C API, see kpcore.h).
enum class UpdateKind : int
{
    MATCH_REFERENCE = 1,     ///< footprint found through its reference designator, not its symbol path
    REFERENCE = 2,           ///< reference designator changes
    VALUE = 3,               ///< value changes
    ASSOCIATION = 4,         ///< the footprint's symbol path changes
    FOOTPRINT_DIFFERS = 5,   ///< footprint library id differs from the symbol's (report only)
    CONNECT = 6,             ///< a pad gets a net
    RECONNECT = 7,           ///< a pad moves to another net
    DISCONNECT = 8,          ///< a pad loses its net
    NO_PIN = 9,              ///< a pad has no pin in the symbol (warning)
    NET_ADD = 10,            ///< a net is added
    NET_RENAME = 11,         ///< a net is renamed in place (tracks, vias and zones follow)
    NET_REMOVE = 12,         ///< an unused net is removed
    NET_ITEMS_STAY = 13,     ///< tracks, vias and zones stay on a net whose pads moved (report only)
    NET_KEPT = 14,           ///< a net without pads is kept because tracks, vias or zones use it
    MISSING_FOOTPRINT = 15,  ///< a symbol has no footprint on the board (report only)
    EXTRA_FOOTPRINT = 16,    ///< a footprint has no symbol (report only, it is kept)
    INCOMPLETE = 17          ///< the schematic is incomplete (sub-sheets missing)
};

enum class UpdateSeverity : int
{
    ACTION = 0,  ///< applied by the update
    INFO = 1,    ///< report only
    WARNING = 2  ///< report only, needs a look
};

struct UpdateLine
{
    UpdateKind     kind = UpdateKind::MATCH_REFERENCE;
    UpdateSeverity severity = UpdateSeverity::INFO;
    int            footprint = -1; ///< board footprint index the line is about, -1 = none
    std::string    text;           ///< translated, one line
};

struct UpdateCounts
{
    int symbols = 0;             ///< symbols that go to the board
    int footprints = 0;          ///< footprints on the board
    int matchedByPath = 0;
    int matchedByReference = 0;
    int referenceChanges = 0;
    int valueChanges = 0;
    int associationChanges = 0;
    int footprintDiffers = 0;
    int padsConnected = 0;       ///< pads that get a net
    int padsReconnected = 0;
    int padsDisconnected = 0;
    int netsAdded = 0;
    int netsRenamed = 0;
    int netsRemoved = 0;
    int missingFootprints = 0;
    int extraFootprints = 0;
    int netsWithItemsLeft = 0;   ///< nets whose tracks, vias or zones stay put although pads moved
    int warnings = 0;
    int changes = 0;             ///< edits an update would make (0 = the board is up to date)
};

/// What an update does, and its description.
struct PcbUpdate
{
    struct PadChange
    {
        int         pad = -1;    ///< index in Board::pads
        std::string net;         ///< new net name, empty = no net
    };

    struct FootprintChange
    {
        int         footprint = -1; ///< index in Board::footprints
        bool        setReference = false, setValue = false, setPath = false;
        std::string reference, value, path;
        std::vector<PadChange> pads;
    };

    struct NetRename
    {
        std::string from, to;
    };

    std::vector<UpdateLine>      lines;
    UpdateCounts                 counts;
    bool                         incomplete = false; ///< sub-sheet files are missing: do not apply
    std::vector<FootprintChange> footprints;
    std::vector<NetRename>       renames;
    std::vector<std::string>     addedNets;   ///< in the order the pads need them
    std::vector<std::string>     removedNets; ///< declared nets that nothing uses any more
};

/// Compare the schematic (with its nets built, see BuildSchematicNets) with the board.
void PlanPcbUpdate( const Schematic& aSch, const Board& aBoard, PcbUpdate& aPlan );

/// Apply a plan to the document in one undo step. aPlan must come from the board in aTarget.
bool ApplyPcbUpdate( EditTarget& aTarget, const PcbUpdate& aPlan, std::string& aError );

/// Plan and apply. False with aError set when there is nothing to change or the update cannot
/// be done (incomplete schematic, a board file this code cannot extend).
bool UpdatePcbFromSchematic( EditTarget& aTarget, const Schematic& aSch, std::string& aError );

/// The plan as text, one line per entry: "<kind> <severity> <text>" (for tests and kpcli).
std::string PcbUpdateText( const PcbUpdate& aPlan );

} // namespace kp

#endif
