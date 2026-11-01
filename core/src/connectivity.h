/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Copper connectivity: which pads, vias, tracks and zone fills of a net touch each other, and
 * the "airwires" (ratsnest) still needed to connect the separate islands of each net.
 */
#ifndef NETLOOM_CONNECTIVITY_H
#define NETLOOM_CONNECTIVITY_H

#include "board.h"

#include <vector>

namespace kp
{

struct Airwire
{
    Pt  a;
    Pt  b;
    int net = 0;
};

struct Connectivity
{
    std::vector<Airwire> airwires;
    int                  netsWithIslands = 0;
};

void ComputeConnectivity( const Board& aBoard, Connectivity& aOut );

} // namespace kp

#endif
