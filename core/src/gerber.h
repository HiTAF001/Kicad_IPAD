/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Reading fabrication files: Gerber (RS-274X, with X2 attributes when present), Excellon drill
 * files and zip archives of them, turned into a board that the viewer can show layer by layer.
 */
#ifndef NETLOOM_GERBER_H
#define NETLOOM_GERBER_H

#include "board.h"

#include <string>
#include <vector>

namespace kp
{

struct FabInput
{
    std::string name;
    std::string data;
};

/// Does this look like a Gerber file, an Excellon drill file or a zip archive?
bool LooksLikeGerber( const char* aData, size_t aLen );
bool LooksLikeExcellon( const char* aData, size_t aLen );
bool LooksLikeZip( const char* aData, size_t aLen );

/// Unpack a zip archive (stored and deflated entries). Folders are flattened.
bool Unzip( const char* aData, size_t aLen, std::vector<FabInput>& aOut, std::string& aError );

/// Board layer a Gerber file belongs to, from its X2 file function or its name, e.g. "F.Cu",
/// "In2.Cu", "B.Mask", "Edge.Cuts"; empty if unknown.
std::string GerberLayerFor( const std::string& aName, const std::string& aFileFunction );

/// Build a board from Gerber and drill files. Throws ParseError if none could be read.
void LoadFabFiles( Board& aBoard, const std::vector<FabInput>& aFiles );

} // namespace kp

#endif
