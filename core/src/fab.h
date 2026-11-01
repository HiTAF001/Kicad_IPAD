/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Fabrication outputs: Gerber X2 per layer, Excellon drill files, bill of materials and
 * component placement (CSV), and a zip with all of them. Plain, fab-neutral files that any
 * board house accepts.
 */
#ifndef NETLOOM_FAB_H
#define NETLOOM_FAB_H

#include "board.h"

#include <string>
#include <vector>

namespace kp
{

struct FabOptions
{
    std::string baseName = "board";  ///< file name prefix
    std::string date;                ///< ISO 8601 creation date (e.g. 2026-11-01T21:00:00+03:00)
    std::string version = "0.5.0";   ///< Netloom version written into the files
    bool        useAuxOrigin = false; ///< coordinates relative to the drill/place origin
    bool        gerbers = true;
    bool        drill = true;
    bool        bom = true;
    bool        positions = true;
    bool        excludeDnpFromPositions = true;
};

struct FabFile
{
    std::string name;
    std::string data;
    std::string kind; ///< "gerber", "drill", "bom", "positions", "job"
    std::string layer; ///< board layer name for Gerbers (canonical), else empty
};

/// Write the selected outputs. Returns false (with a message) if the board can't be made,
/// e.g. it has no outline; aWarnings collects non-fatal notes.
bool WriteFabFiles( const Board& aBoard, const FabOptions& aOptions, std::vector<FabFile>& aOut,
                    std::vector<std::string>& aWarnings );

/// One Gerber file for a board layer (exposed for tests and previews).
std::string GerberForLayer( const Board& aBoard, int aLayer, const FabOptions& aOptions );

/// A zip archive (stored, no compression) of the files.
std::string MakeZip( const std::vector<FabFile>& aFiles, const std::string& aDate );

/// CRC-32 (zip polynomial).
uint32_t Crc32( const void* aData, size_t aLen, uint32_t aCrc = 0 );

} // namespace kp

#endif
