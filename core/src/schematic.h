/*
 * Netloom - PCB design for iPad
 * Copyright (C) 2026 Seyhan Labs
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Schematic model for KiCad 6 .. 10 .kicad_sch files: library symbols embedded in the file,
 * placed symbols, wires, buses, labels, sheets and graphics, plus the sheet hierarchy.
 * Coordinates are millimetres, Y down (library symbols are converted to Y down when read).
 */
#ifndef NETLOOM_SCHEMATIC_H
#define NETLOOM_SCHEMATIC_H

#include "board.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kp
{

using Color = std::array<float, 4>; ///< RGBA 0..1; alpha 0 = use the theme colour

enum class LineStyle : uint8_t { DEFAULT, SOLID, DASH, DOT, DASH_DOT, DASH_DOT_DOT };

/// Fill of a closed shape.
enum class SchFill : uint8_t { NONE, OUTLINE, BACKGROUND, COLOR, HATCH };

struct SchStroke
{
    double    width = 0.0; ///< 0 = default for the item
    LineStyle style = LineStyle::DEFAULT;
    Color     color { 0, 0, 0, 0 };
};

/// Text look (KiCad "effects").
struct SchTextAttr
{
    double sizeX = 1.27, sizeY = 1.27;
    double thickness = 0.0; ///< 0 = default pen
    bool   bold = false, italic = false;
    HAlign hAlign = HAlign::CENTER;
    VAlign vAlign = VAlign::CENTER;
    bool   mirror = false;
    bool   hidden = false;
    Color  color { 0, 0, 0, 0 };
    double lineSpacing = 1.0;
};

struct SchField
{
    std::string name;
    std::string value;
    Pt          pos;
    double      angle = 0.0; ///< degrees
    SchTextAttr attr;
    bool        showName = false;
};

/// A drawing item of a library symbol, or of the sheet itself.
struct SchGraphic
{
    enum Kind : uint8_t { POLY, RECT, CIRCLE, ARC, BEZIER, TEXT, TEXTBOX };

    Kind        kind = POLY;
    Path        pts;     ///< POLY: points; BEZIER: 4 control points; RECT/TEXTBOX: 2 corners
    Pt          center;  ///< CIRCLE
    double      radius = 0.0;
    Pt          start, mid, end; ///< ARC
    double      cornerRadius = 0.0; ///< RECT / TEXTBOX
    SchStroke   stroke;
    SchFill     fill = SchFill::NONE;
    Color       fillColor { 0, 0, 0, 0 };
    int         unit = 0;      ///< library graphics: 0 = all units
    int         bodyStyle = 0; ///< library graphics: 0 = all body styles
    bool        isPrivate = false;

    // TEXT / TEXTBOX
    std::string text;
    Pt          textPos;
    double      textAngle = 0.0;
    SchTextAttr attr;
    double      margins[4] = { 0, 0, 0, 0 }; ///< TEXTBOX: left, top, right, bottom
    int         docItem = -1; ///< sheet graphics: top-level item of the file (parse order)
};

enum class PinType : uint8_t
{
    INPUT, OUTPUT, BIDI, TRISTATE, PASSIVE, UNSPECIFIED, POWER_IN, POWER_OUT, OPEN_COLLECTOR,
    OPEN_EMITTER, NO_CONNECT, FREE
};

enum class PinShape : uint8_t
{
    LINE, INVERTED, CLOCK, INVERTED_CLOCK, INPUT_LOW, CLOCK_LOW, OUTPUT_LOW, FALLING_EDGE_CLOCK,
    NONLOGIC
};

struct LibPin
{
    PinType     type = PinType::PASSIVE;
    PinShape    shape = PinShape::LINE;
    Pt          pos;          ///< connection point (symbol frame, Y down)
    int         orient = 0;   ///< 0 = pin points right (body on the right of pos), 90 up, 180, 270
    double      length = 2.54;
    std::string name;
    std::string number;
    double      nameSize = 1.27;
    double      numberSize = 1.27;
    bool        visible = true;
    int         unit = 0;
    int         bodyStyle = 0;
};

struct LibSymbol
{
    std::string id;       ///< "lib:name" as used by placed symbols
    std::string extends;  ///< parent symbol (derived symbols)
    bool        power = false;
    bool        localPower = false;
    double      pinNameOffset = 0.508; ///< 0 = names outside, over the pin
    bool        showPinNames = true;
    bool        showPinNumbers = true;
    int         unitCount = 1;
    int         bodyStyleCount = 1;
    std::vector<SchField>   fields;
    std::vector<SchGraphic> graphics;
    std::vector<LibPin>     pins;
    std::map<int, std::string> unitNames;
    bool        duplicatePinNumbersAreJumpers = false;
    std::vector<std::vector<std::string>> jumperPinGroups; ///< pin numbers joined inside the part
};

/// Orientation of a placed symbol: screen = pos + M * symbolPoint (KiCad TRANSFORM).
struct SymTransform
{
    int x1 = 1, y1 = 0, x2 = 0, y2 = 1;

    Pt apply( const Pt& p ) const { return Pt( x1 * p.x + y1 * p.y, x2 * p.x + y2 * p.y ); }
};

struct SymbolInstance
{
    std::string path;      ///< "/root-uuid/sheet-uuid..." (without the symbol uuid)
    std::string reference;
    int         unit = 1;
    std::string value, footprint; ///< KiCad 6 per-instance overrides (empty = none)
};

struct SchSymbol
{
    std::string  libId;     ///< key into the screen's library symbols (lib_name if given)
    Pt           pos;
    SymTransform transform;
    int          unit = 1;
    int          bodyStyle = 1;
    bool         dnp = false;
    bool         inBom = true;
    bool         onBoard = true;
    bool         excludeFromSim = false;
    bool         fieldsAutoplaced = false;
    std::string  uuid;
    std::vector<SchField>       fields; ///< positions in the unrotated symbol frame + pos
    std::vector<SymbolInstance> instances;
    std::map<std::string, std::string> pinAlternates; ///< pin number -> alternate name
    int          docItem = -1; ///< top-level item of the file it came from (in parse order)
};

struct SchLine
{
    enum Kind : uint8_t { WIRE, BUS, NOTES };

    Kind      kind = WIRE;
    Pt        a, b;
    SchStroke stroke;
    int       docItem = -1; ///< top-level item of the file it came from (in parse order)
};

struct SchJunction
{
    Pt     pos;
    double diameter = 0.0; ///< 0 = default
    Color  color { 0, 0, 0, 0 };
    int    docItem = -1;
};

struct SchNoConnect
{
    Pt  pos;
    int docItem = -1;
};

struct SchBusEntry
{
    Pt        pos;
    Pt        size { 2.54, 2.54 };
    SchStroke stroke;
    int       docItem = -1;
};

enum class LabelShape : uint8_t { INPUT, OUTPUT, BIDI, TRISTATE, PASSIVE, DOT, ROUND, DIAMOND, RECTANGLE };

enum class Spin : uint8_t { LEFT, UP, RIGHT, BOTTOM };

struct SchLabel
{
    enum Kind : uint8_t { LOCAL, GLOBAL, HIERARCHICAL, DIRECTIVE };

    Kind        kind = LOCAL;
    std::string text;
    Pt          pos;
    Spin        spin = Spin::RIGHT;
    LabelShape  shape = LabelShape::PASSIVE;
    SchTextAttr attr;
    double      pinLength = 2.54; ///< directive labels
    std::vector<SchField> fields;
    std::string uuid;
    int         docItem = -1;
};

struct SchText
{
    std::string text;
    Pt          pos;
    double      angle = 0.0;
    SchTextAttr attr;
    bool        isPrivate = false;
    int         docItem = -1;
};

struct SchImage
{
    Pt     pos;      ///< centre
    double scale = 1.0;
    double width = 0.0, height = 0.0; ///< mm, from the PNG header (0 = unknown)
    int    docItem = -1;
};

struct SchSheetPin
{
    std::string name;
    LabelShape  shape = LabelShape::INPUT;
    Pt          pos;
    Spin        spin = Spin::RIGHT; ///< side of the sheet it sits on, as a label spin
    SchTextAttr attr;
    std::string uuid;
};

struct SchSheet
{
    Pt          pos, size;
    SchStroke   stroke;
    Color       fill { 0, 0, 0, 0 };
    std::vector<SchField>    fields; ///< "Sheetname", "Sheetfile", user fields
    std::vector<SchSheetPin> pins;
    std::string uuid;
    std::string name;      ///< sheet name
    std::string file;      ///< sub-sheet file, relative to this one
    std::map<std::string, std::string> pages; ///< instance path -> page number
    int         docItem = -1;
};

struct TitleBlock
{
    std::string title, date, rev, company;
    std::array<std::string, 9> comments;
};

/// One schematic file (a "screen"); a file used by several sheets is read once.
struct SchScreen
{
    std::string fileName;
    int         sourceFile = -1; ///< index of the file in the list given to LoadSchematic
    int         version = 0;
    std::string generator, generatorVersion;
    std::string uuid;
    std::string paper = "A4";
    double      paperW = 297.0, paperH = 210.0; ///< mm, after portrait
    TitleBlock  titleBlock;

    std::map<std::string, LibSymbol> libSymbols;
    std::vector<SchSymbol>    symbols;
    std::vector<SchLine>      lines;
    std::vector<SchJunction>  junctions;
    std::vector<SchNoConnect> noConnects;
    std::vector<SchBusEntry>  busEntries;
    std::vector<SchLabel>     labels;
    std::vector<SchText>      texts;
    std::vector<SchGraphic>   graphics; ///< shapes and text boxes drawn on the sheet
    std::vector<SchImage>     images;
    std::vector<SchSheet>     sheets;

    /// KiCad 6 root file: references per instance path (newer files store them in symbols).
    std::map<std::string, SymbolInstance> legacySymbolInstances; ///< key: full path incl. symbol
    std::map<std::string, std::string>    legacySheetPages;      ///< sheet path -> page
    std::map<std::string, std::vector<std::string>> busAliases;  ///< (bus_alias) name -> members

    std::vector<std::string> warnings;
};

/// A sheet in the hierarchy: a screen seen through one instance path.
struct SheetNode
{
    int         screen = -1;
    int         parent = -1;  ///< node index
    int         sheetItem = -1; ///< index of the SchSheet in the parent's screen
    std::string path;         ///< "/" for the root, "/uuid/uuid" below
    std::string name;         ///< sheet name ("" for the root)
    std::string page;         ///< page number shown in the title block
    std::vector<int> children;
};

/// Connectivity of one sheet instance: net index per item (-1 = none).
struct SheetNets
{
    std::vector<int> wire;     ///< per SchScreen::lines entry (graphic lines: -1)
    std::vector<int> pin;      ///< per flat pin index (SchematicPinBase)
    std::vector<int> label;    ///< per SchScreen::labels entry
    std::vector<int> junction; ///< per SchScreen::junctions entry
    std::vector<int> sheetPin; ///< per sheet pin, sheets in order, pins in order
    std::vector<int> noConnect; ///< per SchScreen::noConnects entry
    std::vector<int> busEntry; ///< per SchScreen::busEntries entry
};

struct SchNet
{
    std::string name;
    bool        global = false;  ///< named by a global label or power symbol
    bool        noConnect = false;
    bool        bus = false;     ///< a bus (its members are nets of their own)
    int         pinCount = 0;    ///< pins of real (not power) symbols
};

struct Schematic
{
    std::vector<SchScreen> screens;
    std::vector<SheetNode> sheets; ///< sheets[0] = root
    std::vector<std::string> warnings;
    std::vector<std::string> missingFiles; ///< sub-sheet files that were not provided

    std::vector<SchNet>    nets;     ///< filled by BuildSchematicNets
    std::vector<SheetNets> sheetNets; ///< per sheets[] entry

    /// Reference shown for a symbol on a sheet (instance data, else the Reference field).
    std::string Reference( const SheetNode& aSheet, const SchSymbol& aSymbol ) const;
    int         Unit( const SheetNode& aSheet, const SchSymbol& aSymbol ) const;
    /// Value of a field as shown on a sheet (KiCad 6 per-instance Value / Footprint included).
    std::string FieldValue( const SheetNode& aSheet, const SchSymbol& aSymbol, const SchField& aField ) const;
    const LibSymbol* Lib( const SchScreen& aScreen, const SchSymbol& aSymbol ) const;
};

struct SchFile
{
    std::string name; ///< path relative to the project folder, e.g. "sub/power.kicad_sch"
    std::string data;
};

/// Is this the start of a .kicad_sch file?
bool LooksLikeSchematic( const char* aData, size_t aLen );

/// Parse one .kicad_sch file. Throws ParseError on fatal errors.
void ParseSchematicFile( SchScreen& aScreen, const char* aData, size_t aLen );

/// Load a schematic: aFiles[aRoot] is the root sheet, the others are candidates for the
/// sub-sheets it references (matched by relative path, then by file name).
void LoadSchematic( Schematic& aSch, const std::vector<SchFile>& aFiles, size_t aRoot );

/// Sub-sheet file names a screen refers to (to know which files to read).
std::vector<std::string> SheetFileNames( const SchScreen& aScreen );

/// Paper size in mm for a KiCad paper name ("A4", "USLetter", ...); false if unknown.
bool PaperSize( const std::string& aName, double& aW, double& aH );

/// Connect the whole hierarchy into nets (KiCad rules: wires, junctions, labels, power
/// symbols, sheet pins and hierarchical labels, bus members through sheets) and name them.
void BuildSchematicNets( Schematic& aSch );

/// Members of a bus label: "D[0..3]" -> D0..D3, "SPI{SCK MOSI}" -> SPI.SCK, SPI.MOSI; a plain
/// name gives itself.
std::vector<std::string> BusMembers( const std::string& aLabel );

/// Netlist as text, one net per line: "name<TAB>ref:pin ref:pin ..." (pins of symbols on the
/// board, as in KiCad's netlist export; nets without such pins are left out).
std::string SchematicNetlistText( const Schematic& aSch );

/// "/" for the root, "/sub/" for a sub-sheet (KiCad's human readable sheet path).
std::string SheetPathName( const Schematic& aSch, int aSheet );

struct Mesh;

/// Build the render mesh of one sheet (index into aSch.sheets), KiCad's default colours.
void BuildSchematicMesh( const Schematic& aSch, int aSheet, Mesh& aMesh );

/// First flat pin index (ItemType::SCH_PIN) of every symbol of a screen; the pins of symbol
/// s are base[s] + index in its library symbol's pin list. base.back() = total.
std::vector<int> SchematicPinBase( const Schematic& aSch, const SchScreen& aScreen );

/// Position of a pin's connection point and its body end on the sheet.
void SchematicPinPoints( const SchSymbol& aSymbol, const LibPin& aPin, Pt& aTip, Pt& aRoot );

/// Reference with the unit letter for multi-unit symbols ("U1A").
std::string SchematicShownReference( const Schematic& aSch, const SheetNode& aSheet, const SchSymbol& aSymbol );

} // namespace kp

#endif
