/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Design rule check: the checks a board needs before it goes to a fab.
 *
 * Rules come from the project file (KiCad 6+ keeps them in the .kicad_pro next to the board),
 * from the board file itself (KiCad 4/5 net classes and setup), or KiCad's defaults. Results
 * use KiCad's violation names and severities so they read the same in both programs.
 */
#ifndef NETLOOM_DRC_H
#define NETLOOM_DRC_H

#include "board.h"
#include "connectivity.h"
#include "geom.h"

#include <string>
#include <vector>

namespace kp
{

enum class Severity : uint8_t
{
    ERROR,
    WARNING,
    IGNORE
};

enum class DrcCode : uint8_t
{
    CLEARANCE,
    SHORTING_ITEMS,
    TRACKS_CROSSING,
    HOLE_CLEARANCE,
    EDGE_CLEARANCE,
    TRACK_WIDTH,
    VIA_DIAMETER,
    ANNULAR_WIDTH,
    DRILL_OUT_OF_RANGE,
    MICROVIA_DRILL_OUT_OF_RANGE,
    HOLE_NEAR_HOLE,
    HOLES_CO_LOCATED,
    COURTYARDS_OVERLAP,
    MALFORMED_COURTYARD,
    UNCONNECTED_ITEMS,
    TRACK_DANGLING,
    VIA_DANGLING,
    INVALID_OUTLINE,
    COUNT
};

/// KiCad's name for the check (as in .kicad_pro "rule_severities" and DRC reports).
const char* DrcKey( DrcCode aCode );

/// Short English title (translatable).
const char* DrcTitle( DrcCode aCode );

Severity DefaultSeverity( DrcCode aCode );

struct NetClassRule
{
    std::string name = "Default";
    double      clearance = 0.2;
    double      trackWidth = 0.2;
    double      viaDiameter = 0.6;
    double      viaDrill = 0.3;
    double      uviaDiameter = 0.3;
    double      uviaDrill = 0.1;
};

struct DesignRules
{
    enum class Source : uint8_t
    {
        DEFAULTS,
        BOARD,
        PROJECT
    };

    double minClearance = 0.0;
    double minTrackWidth = 0.2;
    double minViaDiameter = 0.5;
    double minViaAnnular = 0.1;
    double minThroughHole = 0.3;
    double minMicroviaDiameter = 0.2;
    double minMicroviaDrill = 0.1;
    double minHoleToHole = 0.25;
    double minHoleClearance = 0.25;
    double copperEdgeClearance = 0.5;

    std::vector<NetClassRule> classes { NetClassRule() }; ///< [0] = Default
    std::vector<int>          netClass;                   ///< per board net: index into classes
    Severity                  severity[(int) DrcCode::COUNT];
    Source                    source = Source::DEFAULTS;
    bool                      reportAllTrackErrors = false; ///< else one per track and layer (KiCad default)

    DesignRules();

    const NetClassRule& ClassOf( int aNet ) const;
};

/// Rules for a board. aProject is the .kicad_pro text (may be null). aNote receives a short
/// explanation when the project file could not be used.
DesignRules ResolveRules( const Board& aBoard, const char* aProject, size_t aProjectLen, std::string& aNote );

struct DrcRef
{
    ItemType type = ItemType::NONE;
    int      index = -1;
};

struct DrcItem
{
    DrcCode  code = DrcCode::CLEARANCE;
    Severity severity = Severity::ERROR;
    Pt       pos;              ///< where to put the marker
    int      layer = -1;       ///< board layer, -1 = not layer specific
    DrcRef   a, b;             ///< items involved (b may be empty)
    double   actual = -1.0;    ///< measured value, mm (-1 = n/a)
    double   required = -1.0;  ///< rule value, mm (-1 = n/a)
    int      netA = 0, netB = 0;
};

/// Run all checks. Items are sorted: errors first, then by kind and position.
void RunDrc( const Board& aBoard, const DesignRules& aRules, const Connectivity& aConn,
             std::vector<DrcItem>& aOut );

/// One line describing the violation (translated), e.g.
/// "Clearance 0.150 mm (rule 0.200 mm): track GND / pad 3 of U1".
std::string DrcMessage( const Board& aBoard, const DrcItem& aItem );

/// Short description of an item, e.g. "Track (GND) on F.Cu", "Pad 3 of U1".
std::string DescribeItem( const Board& aBoard, const DrcRef& aRef );

} // namespace kp

#endif
