// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.
//
// Swift wrapper around the C core: loads a board, copies its mesh into Metal buffers and keeps
// the metadata the UI needs. Picking, descriptions and highlight ranges are forwarded to the
// core (call those from the main thread only).

import Foundation
import Metal
import simd

enum NetloomError: LocalizedError {
    case message(String)

    var errorDescription: String? {
        switch self {
        case .message(let text): return text
        }
    }
}

/// Item kinds, matching KP_KIND_* in kpcore.h.
enum ItemKind: Int, CaseIterable {
    case pad = 0, via, track, graphic, text, zone, wallPTH, wallNPTH, wallVia, hole
    case label0, label1, label2, label3, label4
}

/// Item types, matching KP_ITEM_* in kpcore.h.
enum ItemType: Int32 {
    case nothing = 0, pad, via, track, arc, footprint, zone, shape, text
    case schSymbol = 9, schPin, schWire, schJunction, schLabel, schSheet, schNote, schSheetPin
}

/// What a LoadedBoard shows (KP_KIND_* in kpcore.h).
enum BoardKind {
    case pcb, gerber, schematic
}

/// One sheet of a schematic hierarchy.
struct SheetEntry: Identifiable, Equatable {
    let id: Int          // index in the hierarchy (0 = root)
    let parent: Int      // -1 for the root
    let sheetItem: Int   // the sheet box in the parent sheet
    let depth: Int
    let name: String
    let page: String
    let file: String

    var title: String { name.isEmpty ? String(localized: "Root sheet") : name }
}

/// A symbol somewhere in a schematic hierarchy (for search).
struct SchematicSymbolEntry: Identifiable {
    let id: Int
    let sheet: Int
    let index: Int
    let reference: String
    let value: String
}

/// Object categories the user can toggle.
enum ObjectCategory: String, CaseIterable, Identifiable {
    case tracks, vias, pads, zones, graphics, texts, holes, labels, ratsnest

    var id: String { rawValue }

    /// Pseudo item kind for the airwires (not a mesh kind, ignored by picking).
    static let ratsnestKind = 100

    var title: String {
        switch self {
        case .tracks: return String(localized: "Tracks")
        case .vias: return String(localized: "Vias")
        case .pads: return String(localized: "Pads")
        case .zones: return String(localized: "Copper zones")
        case .graphics: return String(localized: "Graphics")
        case .texts: return String(localized: "Texts")
        case .holes: return String(localized: "Holes")
        case .labels: return String(localized: "Pad numbers and net names")
        case .ratsnest: return String(localized: "Unconnected (ratsnest)")
        }
    }

    var systemImage: String {
        switch self {
        case .tracks: return "point.topleft.down.curvedto.point.bottomright.up"
        case .vias: return "circle.circle"
        case .pads: return "square.fill"
        case .zones: return "square.fill.on.square.fill"
        case .graphics: return "pencil.and.outline"
        case .texts: return "textformat"
        case .holes: return "smallcircle.filled.circle"
        case .labels: return "number.square"
        case .ratsnest: return "line.diagonal"
        }
    }

    var kinds: [Int] {
        switch self {
        case .tracks: return [ItemKind.track.rawValue]
        case .vias: return [ItemKind.via.rawValue]
        case .pads: return [ItemKind.pad.rawValue]
        case .zones: return [ItemKind.zone.rawValue]
        case .graphics: return [ItemKind.graphic.rawValue]
        case .texts: return [ItemKind.text.rawValue]
        case .holes: return [ItemKind.wallPTH.rawValue, ItemKind.wallNPTH.rawValue,
                             ItemKind.wallVia.rawValue, ItemKind.hole.rawValue]
        case .labels: return [ItemKind.label0.rawValue, ItemKind.label1.rawValue, ItemKind.label2.rawValue,
                              ItemKind.label3.rawValue, ItemKind.label4.rawValue]
        case .ratsnest: return [ObjectCategory.ratsnestKind]
        }
    }
}

/// Layer slot kinds, matching KP_LAYER_* in kpcore.h.
enum LayerKind: Int {
    case copper = 0, silk, mask, paste, adhesive, fab, courtyard, edge, margin, user, virtualSlot
}

struct LayerSlot: Identifiable {
    let id: Int
    let name: String
    let display: String
    let kind: LayerKind
    let side: Int
    let sortFront: Int
    let sortBack: Int
    let color: SIMD4<Float>
    let defaultVisible: Bool
    let indexCount: UInt32
    /// Label slots: shown only together with this layer slot.
    let parentSlot: Int
    /// +1 = only in the top view, -1 = only in the bottom view, 0 = always.
    let viewSide: Int

    var hasContent: Bool { indexCount > 0 }

    /// Name shown in the layer list: user name, plus the canonical one when different.
    var title: String { display.isEmpty ? name : display }
    var subtitle: String? { (display.isEmpty || display == name || name.hasPrefix("Sch.")) ? nil : name }
}

struct DrawGroup {
    let slot: Int
    let kind: Int
    let color: SIMD4<Float>
    let indexStart: UInt32
    let indexCount: UInt32
    let minZoom: Float
}

struct BoardBounds: Equatable {
    var minX: Double
    var minY: Double
    var maxX: Double
    var maxY: Double

    var width: Double { maxX - minX }
    var height: Double { maxY - minY }
    var midX: Double { (minX + maxX) / 2 }
    var midY: Double { (minY + maxY) / 2 }
}

/// A selectable thing on the board (mirrors kp_item).
struct PickedItem: Equatable, Hashable {
    var type: Int32
    var index: Int32
    var footprint: Int32
    var net: Int32
    var layer: Int32

    init(type: Int32, index: Int32, footprint: Int32 = -1, net: Int32 = 0, layer: Int32 = -1) {
        self.type = type
        self.index = index
        self.footprint = footprint
        self.net = net
        self.layer = layer
    }

    init(_ c: kp_item) {
        self.init(type: c.type, index: c.index, footprint: c.footprint, net: c.net, layer: c.layer)
    }

    /// A whole net, without a particular item.
    static func net(_ net: Int32) -> PickedItem {
        PickedItem(type: ItemType.nothing.rawValue, index: -1, net: net)
    }

    static func footprint(_ index: Int32) -> PickedItem {
        PickedItem(type: ItemType.footprint.rawValue, index: index, footprint: index)
    }

    /// A placed symbol of a schematic sheet.
    static func symbol(_ index: Int32) -> PickedItem {
        PickedItem(type: ItemType.schSymbol.rawValue, index: index)
    }

    var itemType: ItemType { ItemType(rawValue: type) ?? .nothing }

    var cItem: kp_item {
        kp_item(type: type, index: index, footprint: footprint, net: net, layer: layer)
    }
}

struct InfoRow: Identifiable, Equatable {
    let id: Int
    let key: String
    let value: String
}

/// Description produced by the core: a title and key/value rows.
struct ItemDescription: Equatable {
    let title: String
    let rows: [InfoRow]

    static let empty = ItemDescription(title: "", rows: [])

    init(title: String, rows: [InfoRow]) {
        self.title = title
        self.rows = rows
    }

    /// Parses "Title\nKey\tValue\n...".
    init(parsing text: String) {
        var lines = text.components(separatedBy: "\n")
        title = lines.isEmpty ? "" : lines.removeFirst()
        rows = lines.enumerated().compactMap { offset, line in
            let parts = line.components(separatedBy: "\t")
            guard parts.count >= 2 else { return nil }
            return InfoRow(id: offset, key: parts[0], value: parts.dropFirst().joined(separator: "\t"))
        }
    }

    func value(for key: String) -> String? {
        rows.first { $0.key == key }?.value
    }
}

struct FootprintEntry: Identifiable {
    let id: Int
    let reference: String
    let value: String
    let back: Bool
}

struct NetEntry: Identifiable {
    let id: Int
    let name: String
}

final class MetalContext: @unchecked Sendable {
    static let shared = MetalContext()

    let device: MTLDevice?

    private init() {
        device = MTLCreateSystemDefaultDevice()
    }
}

final class LoadedBoard: @unchecked Sendable {
    let name: String
    let fileSize: Int
    let stats: kp_stats
    let generator: String
    let generatorVersion: String
    let title: String
    let revision: String
    let company: String
    let date: String
    let slots: [LayerSlot]
    let groups: [DrawGroup]
    let bounds: BoardBounds
    let warnings: [String]
    let footprints: [FootprintEntry]
    let nets: [NetEntry]
    let vertexBuffer: MTLBuffer?
    let indexBuffer: MTLBuffer?
    /// Airwires (ratsnest) as start/end points in millimetres.
    let airwires: [SIMD4<Float>]
    /// Boards rebuilt from the same document share this (0 = not editable).
    let lineage: Int
    /// Built from Gerber / drill files: for viewing only.
    let isFabrication: Bool
    let kind: BoardKind
    /// Canvas colour (dark for boards, KiCad's paper colour for schematics).
    let background: SIMD4<Float>
    /// Schematics: the open sheet and the whole hierarchy.
    let sheetIndex: Int
    let sheets: [SheetEntry]
    let missingSheetFiles: [String]

    var isSchematic: Bool { kind == .schematic }
    var currentSheet: SheetEntry? { sheetIndex < sheets.count ? sheets[sheetIndex] : nil }

    private let handle: OpaquePointer

    private init(name: String, fileSize: Int, handle: OpaquePointer, device: MTLDevice, lineage: Int) throws {
        self.name = name
        self.fileSize = fileSize
        self.handle = handle
        self.lineage = lineage
        isFabrication = kp_board_is_fab(handle) != 0

        switch kp_board_kind(handle) {
        case Int32(KP_KIND_SCHEMATIC): kind = .schematic
        case Int32(KP_KIND_GERBER): kind = .gerber
        default: kind = .pcb
        }

        var rgba = [Float](repeating: 0, count: 4)
        kp_board_background(handle, &rgba)
        background = SIMD4<Float>(rgba[0], rgba[1], rgba[2], 1)

        var sheetList: [SheetEntry] = []
        var missing: [String] = []

        if kind == .schematic {
            for i in 0..<Int(kp_sch_sheet_count(handle)) {
                var info = kp_sheet_info()
                kp_sch_sheet_info(handle, Int32(i), &info)
                sheetList.append(SheetEntry(id: i,
                                            parent: Int(info.parent),
                                            sheetItem: Int(info.sheet_item),
                                            depth: Int(info.depth),
                                            name: kp_sch_sheet_name(handle, Int32(i)).map { String(cString: $0) } ?? "",
                                            page: kp_sch_sheet_page(handle, Int32(i)).map { String(cString: $0) } ?? "",
                                            file: kp_sch_sheet_file(handle, Int32(i)).map { String(cString: $0) } ?? ""))
            }

            for i in 0..<Int(kp_sch_missing_count(handle)) {
                missing.append(kp_sch_missing_file(handle, Int32(i)).map { String(cString: $0) } ?? "")
            }
        }

        sheets = sheetList
        missingSheetFiles = missing
        sheetIndex = kind == .schematic ? Int(kp_sch_current_sheet(handle)) : 0

        var s = kp_stats()
        kp_board_stats(handle, &s)
        stats = s

        func str(_ key: Int) -> String {
            guard let p = kp_board_string(handle, Int32(key)) else { return "" }
            return String(cString: p)
        }

        generator = str(Int(KP_STR_GENERATOR))
        generatorVersion = str(Int(KP_STR_GENERATOR_VERSION))
        title = str(Int(KP_STR_TITLE))
        revision = str(Int(KP_STR_REVISION))
        company = str(Int(KP_STR_COMPANY))
        date = str(Int(KP_STR_DATE))

        var slotList: [LayerSlot] = []
        let slotCount = Int(kp_board_slot_count(handle))

        for i in 0..<slotCount {
            var info = kp_slot_info()
            kp_board_slot_info(handle, Int32(i), &info)
            let slotName = kp_board_slot_name(handle, Int32(i)).map { String(cString: $0) } ?? ""
            let display = kp_board_slot_display(handle, Int32(i)).map { String(cString: $0) } ?? ""
            slotList.append(LayerSlot(id: i,
                                      name: slotName,
                                      display: display,
                                      kind: LayerKind(rawValue: Int(info.kind)) ?? .user,
                                      side: Int(info.side),
                                      sortFront: Int(info.sort_front),
                                      sortBack: Int(info.sort_back),
                                      color: SIMD4<Float>(info.r, info.g, info.b, info.a),
                                      defaultVisible: info.default_visible != 0,
                                      indexCount: info.index_count,
                                      parentSlot: Int(info.parent_slot),
                                      viewSide: Int(info.view_side)))
        }

        slots = slotList

        var groupList: [DrawGroup] = []
        let groupCount = Int(kp_board_group_count(handle))
        groupList.reserveCapacity(groupCount)

        for i in 0..<groupCount {
            var g = kp_group()
            kp_board_group(handle, Int32(i), &g)
            groupList.append(DrawGroup(slot: Int(g.slot),
                                       kind: Int(g.kind),
                                       color: SIMD4<Float>(g.r, g.g, g.b, g.a),
                                       indexStart: g.index_start,
                                       indexCount: g.index_count,
                                       minZoom: g.min_zoom))
        }

        groups = groupList

        var x0 = 0.0, y0 = 0.0, x1 = 100.0, y1 = 100.0
        kp_board_bounds(handle, &x0, &y0, &x1, &y1)
        bounds = BoardBounds(minX: x0, minY: y0, maxX: x1, maxY: y1)

        var warn: [String] = []

        for i in 0..<Int(s.warning_count) {
            if let p = kp_board_warning(handle, Int32(i)) {
                warn.append(String(cString: p))
            }
        }

        warnings = warn

        var fpList: [FootprintEntry] = []
        let fpCount = Int(kp_board_footprint_count(handle))
        fpList.reserveCapacity(fpCount)

        for i in 0..<fpCount {
            let ref = kp_board_footprint_ref(handle, Int32(i)).map { String(cString: $0) } ?? ""
            let value = kp_board_footprint_value(handle, Int32(i)).map { String(cString: $0) } ?? ""
            fpList.append(FootprintEntry(id: i, reference: ref, value: value,
                                         back: kp_board_footprint_back(handle, Int32(i)) != 0))
        }

        footprints = fpList

        var netList: [NetEntry] = []
        let netCount = Int(kp_board_net_count(handle))

        for i in 1..<max(netCount, 1) {
            let n = kp_board_net_name(handle, Int32(i)).map { String(cString: $0) } ?? ""

            if !n.isEmpty {
                netList.append(NetEntry(id: i, name: n))
            }
        }

        nets = netList

        var wireFloats = 0
        var wires: [SIMD4<Float>] = []

        if let wp = kp_board_airwires(handle, &wireFloats), wireFloats >= 4 {
            wires.reserveCapacity(wireFloats / 4)

            for i in stride(from: 0, to: wireFloats - 3, by: 4) {
                wires.append(SIMD4(wp[i], wp[i + 1], wp[i + 2], wp[i + 3]))
            }
        }

        airwires = wires

        // Upload the mesh to the GPU, then free the CPU copy. All throwing happens before the
        // last stored properties are set, so deinit (which frees the handle) never runs for a
        // half-built object; the caller frees the handle in that case.
        var vertexFloats = 0
        var indexCount = 0
        let vptr = kp_board_vertices(handle, &vertexFloats)
        let iptr = kp_board_indices(handle, &indexCount)
        var vb: MTLBuffer?
        var ib: MTLBuffer?

        if let vptr, let iptr, vertexFloats > 0, indexCount > 0 {
            let vBytes = vertexFloats * MemoryLayout<Float>.stride
            let iBytes = indexCount * MemoryLayout<UInt32>.stride

            if vBytes > device.maxBufferLength || iBytes > device.maxBufferLength {
                throw NetloomError.message(String(localized:
                    "The board is too large for this device's graphics memory (\(vBytes / 1_048_576) MB vertices, \(iBytes / 1_048_576) MB indices)."))
            }

            vb = device.makeBuffer(bytes: vptr, length: vBytes, options: .storageModeShared)
            ib = device.makeBuffer(bytes: iptr, length: iBytes, options: .storageModeShared)

            if vb == nil || ib == nil {
                throw NetloomError.message(String(localized: "Could not allocate graphics memory (the board may be too large)."))
            }
        }

        vertexBuffer = vb
        indexBuffer = ib
        kp_board_release_mesh(handle)
    }

    deinit {
        kp_board_free(handle)
    }

    static func load(data: Data, name: String) throws -> LoadedBoard {
        guard let device = MetalContext.shared.device else {
            throw NetloomError.message(String(localized: "This device does not support Metal graphics."))
        }

        let errorSize = 1024
        var errorBuffer = [CChar](repeating: 0, count: errorSize)

        let handle: OpaquePointer? = data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> OpaquePointer? in
            guard let base = raw.baseAddress, raw.count > 0 else { return nil }
            return errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
                kp_board_load(base.assumingMemoryBound(to: CChar.self), raw.count,
                              errPtr.baseAddress, errorSize)
            }
        }

        guard let handle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The file could not be opened.") : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: data.count, handle: handle, device: device, lineage: 0)
        } catch {
            // init threw before taking ownership through deinit
            kp_board_free(handle)
            throw error
        }
    }

    /// Is this a KiCad schematic (.kicad_sch)?
    static func looksLikeSchematic(_ data: Data) -> Bool {
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            guard let base = raw.baseAddress, raw.count > 0 else { return false }
            return kp_data_is_schematic(base.assumingMemoryBound(to: CChar.self), raw.count) != 0
        }
    }

    /// Builds the root sheet of a schematic from its files (names relative to the project
    /// folder); the root is guessed when not given (background task).
    static func loadSchematic(files: [(name: String, data: Data)], root: Int? = nil, name: String) throws -> LoadedBoard {
        guard let device = MetalContext.shared.device else {
            throw NetloomError.message(String(localized: "This device does not support Metal graphics."))
        }

        guard let input = kp_fab_input_new() else {
            throw NetloomError.message(String(localized: "Out of memory."))
        }

        defer { kp_fab_input_free(input) }

        var total = 0

        for file in files {
            total += file.data.count
            file.data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                guard let base = raw.baseAddress, raw.count > 0 else { return }
                kp_fab_input_add(input, file.name, base.assumingMemoryBound(to: CChar.self), raw.count)
            }
        }

        let rootIndex = root ?? Int(kp_sch_guess_root(input))

        guard rootIndex >= 0 else {
            throw NetloomError.message(String(localized: "No schematic file was found."))
        }

        let errorSize = 1024
        var errorBuffer = [CChar](repeating: 0, count: errorSize)
        let handle = errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
            kp_board_load_sch(input, Int32(rootIndex), 0, errPtr.baseAddress, errorSize)
        }

        guard let handle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The file could not be opened.") : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: total, handle: handle, device: device, lineage: 0)
        } catch {
            kp_board_free(handle)
            throw error
        }
    }

    /// Another sheet of the same schematic (no parsing; background task).
    func openSheet(_ index: Int) throws -> LoadedBoard {
        guard let device = MetalContext.shared.device else {
            throw NetloomError.message(String(localized: "This device does not support Metal graphics."))
        }

        let errorSize = 512
        var errorBuffer = [CChar](repeating: 0, count: errorSize)
        let newHandle = errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
            kp_board_sch_open_sheet(handle, Int32(index), errPtr.baseAddress, errorSize)
        }

        guard let newHandle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The sheet could not be opened.") : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: fileSize, handle: newHandle, device: device, lineage: 0)
        } catch {
            kp_board_free(newHandle)
            throw error
        }
    }

    /// Is this a Gerber, Excellon drill or zip file rather than a KiCad board?
    static func looksLikeFabrication(_ data: Data) -> Bool {
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Bool in
            guard let base = raw.baseAddress, raw.count > 0 else { return false }
            return kp_data_is_fab(base.assumingMemoryBound(to: CChar.self), raw.count) != 0
        }
    }

    /// Builds a view-only board from Gerber, drill and zip files (background task).
    static func loadFabrication(files: [(name: String, data: Data)], name: String) throws -> LoadedBoard {
        guard let device = MetalContext.shared.device else {
            throw NetloomError.message(String(localized: "This device does not support Metal graphics."))
        }

        guard let input = kp_fab_input_new() else {
            throw NetloomError.message(String(localized: "Out of memory."))
        }

        defer { kp_fab_input_free(input) }

        var total = 0

        for file in files {
            total += file.data.count
            file.data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                guard let base = raw.baseAddress, raw.count > 0 else { return }
                kp_fab_input_add(input, file.name, base.assumingMemoryBound(to: CChar.self), raw.count)
            }
        }

        let errorSize = 1024
        var errorBuffer = [CChar](repeating: 0, count: errorSize)
        let handle = errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
            kp_board_load_fab(input, errPtr.baseAddress, errorSize)
        }

        guard let handle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The files could not be opened.") : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: total, handle: handle, device: device, lineage: 0)
        } catch {
            kp_board_free(handle)
            throw error
        }
    }

    /// Builds a board from the current state of an editable document (background task).
    static func load(document: BoardDocument, name: String, fileSize: Int) throws -> LoadedBoard {
        guard let device = MetalContext.shared.device else {
            throw NetloomError.message(String(localized: "This device does not support Metal graphics."))
        }

        let errorSize = 1024
        var errorBuffer = [CChar](repeating: 0, count: errorSize)
        let handle = errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
            kp_board_load_doc(document.handle, errPtr.baseAddress, errorSize)
        }

        guard let handle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The board could not be built.") : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: fileSize, handle: handle, device: device,
                                   lineage: document.lineage)
        } catch {
            kp_board_free(handle)
            throw error
        }
    }

    // MARK: - Queries (main thread)

    /// The sheet a sheet box of the open sheet leads to.
    func childSheet(forSheetItem item: Int) -> Int? {
        let child = Int(kp_sch_sheet_for_item(handle, Int32(item)))
        return child >= 0 ? child : nil
    }

    /// Sheets on which a net appears.
    func netSheets(_ net: Int32) -> [Int] {
        let count = Int(kp_sch_net_sheets(handle, net, nil, 0))

        guard count > 0 else { return [] }

        var out = [Int32](repeating: 0, count: count)
        _ = out.withUnsafeMutableBufferPointer { o in
            kp_sch_net_sheets(handle, net, o.baseAddress, Int32(count))
        }
        return out.map { Int($0) }
    }

    /// Every symbol of the hierarchy (power symbols left out).
    private(set) lazy var schematicSymbols: [SchematicSymbolEntry] = {
        var list: [SchematicSymbolEntry] = []

        for i in 0..<Int(kp_sch_symbol_count(handle)) {
            var sheet: Int32 = 0
            var index: Int32 = 0
            var ref: UnsafePointer<CChar>?
            var value: UnsafePointer<CChar>?

            guard kp_sch_symbol_entry(handle, Int32(i), &sheet, &index, &ref, &value) != 0 else { continue }

            let reference = ref.map { String(cString: $0) } ?? ""

            if reference.hasPrefix("#") {
                continue
            }

            list.append(SchematicSymbolEntry(id: i, sheet: Int(sheet), index: Int(index), reference: reference,
                                             value: value.map { String(cString: $0) } ?? ""))
        }

        return list
    }()

    func pick(at point: SIMD2<Double>, tolerance: Double, visible: [UInt8], hiddenKinds: UInt32,
              flipped: Bool) -> [PickedItem] {
        var out = [kp_item](repeating: kp_item(), count: 16)
        let capacity = Int32(out.count)

        let n = visible.withUnsafeBufferPointer { vis -> Int32 in
            out.withUnsafeMutableBufferPointer { o -> Int32 in
                kp_board_pick(handle, point.x, point.y, tolerance, vis.baseAddress, Int32(vis.count),
                              hiddenKinds, flipped ? 1 : 0, o.baseAddress, capacity)
            }
        }

        return out.prefix(Int(max(n, 0))).map { PickedItem($0) }
    }

    func snap(_ point: SIMD2<Double>, tolerance: Double, visible: [UInt8]) -> SIMD2<Double>? {
        var x = 0.0
        var y = 0.0

        let found = visible.withUnsafeBufferPointer { vis -> Int32 in
            kp_board_snap(handle, point.x, point.y, tolerance, vis.baseAddress, Int32(vis.count), &x, &y)
        }

        return found != 0 ? SIMD2(x, y) : nil
    }

    func describe(_ item: PickedItem) -> ItemDescription {
        if item.itemType == .nothing {
            return describeNet(item.net)
        }

        var c = item.cItem
        guard let p = kp_board_item_info(handle, &c) else { return .empty }
        return ItemDescription(parsing: String(cString: p))
    }

    func describeNet(_ net: Int32) -> ItemDescription {
        guard net > 0, let p = kp_board_net_info(handle, net) else { return .empty }
        return ItemDescription(parsing: String(cString: p))
    }

    func bounds(of item: PickedItem, wholeNet: Bool) -> BoardBounds? {
        var c = item.cItem
        var x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0

        guard kp_board_item_bounds(handle, &c, wholeNet ? 1 : 0, &x0, &y0, &x1, &y1) != 0 else {
            return nil
        }

        return BoardBounds(minX: x0, minY: y0, maxX: x1, maxY: y1)
    }

    func ranges(of item: PickedItem, wholeNet: Bool) -> [kp_range] {
        var c = item.cItem
        let total = Int(kp_board_item_ranges(handle, &c, wholeNet ? 1 : 0, nil, 0))

        guard total > 0 else { return [] }

        var out = [kp_range](repeating: kp_range(), count: total)
        out.withUnsafeMutableBufferPointer { o in
            _ = kp_board_item_ranges(handle, &c, wholeNet ? 1 : 0, o.baseAddress, Int32(total))
        }
        return out
    }

    /// Anchor position (mm) and orientation (degrees) of a footprint.
    func footprintPose(_ index: Int) -> (position: SIMD2<Double>, angle: Double)? {
        var x = 0.0, y = 0.0, a = 0.0

        guard kp_board_footprint_pose(handle, Int32(index), &x, &y, &a) != 0 else { return nil }

        return (SIMD2(x, y), a)
    }

    func footprintPadCount(_ index: Int) -> Int {
        Int(kp_board_footprint_pad_count(handle, Int32(index)))
    }

    func netPadCount(_ net: Int) -> Int {
        Int(kp_board_net_pad_count(handle, Int32(net)))
    }

    func trackWidth(_ item: PickedItem) -> Double {
        var c = item.cItem
        return kp_board_track_width(handle, &c)
    }

    func viaGeometry(_ index: Int) -> (size: Double, drill: Double)? {
        var size = 0.0, drill = 0.0

        guard kp_board_via_geometry(handle, Int32(index), &size, &drill) != 0 else { return nil }

        return (size, drill)
    }

    // MARK: - Edits (main thread; the document must not be in use elsewhere)

    func isCurrent(for document: BoardDocument) -> Bool {
        kp_doc_board_current(document.handle, handle) != 0
    }

    /// Runs one core edit; throws its message on failure.
    private func edit(_ document: BoardDocument,
                      _ body: (OpaquePointer, OpaquePointer, UnsafeMutablePointer<CChar>?, Int) -> Int32) throws {
        let errorSize = 512
        var errorBuffer = [CChar](repeating: 0, count: errorSize)
        let ok = errorBuffer.withUnsafeMutableBufferPointer { errPtr -> Int32 in
            body(document.handle, handle, errPtr.baseAddress, errorSize)
        }

        if ok == 0 {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw NetloomError.message(message.isEmpty ? String(localized: "The edit could not be made.") : message)
        }
    }

    func moveFootprint(_ index: Int, by delta: SIMD2<Double>, in document: BoardDocument) throws {
        try edit(document) { doc, board, err, size in
            kp_edit_move_footprint(doc, board, Int32(index), delta.x, delta.y, err, size)
        }
    }

    func rotateFootprint(_ index: Int, degrees: Double, in document: BoardDocument) throws {
        try edit(document) { doc, board, err, size in
            kp_edit_rotate_footprint(doc, board, Int32(index), degrees, err, size)
        }
    }

    func delete(_ item: PickedItem, in document: BoardDocument) throws {
        var c = item.cItem
        try edit(document) { doc, board, err, size in
            kp_edit_delete(doc, board, &c, err, size)
        }
    }

    func setReferenceAndValue(_ index: Int, reference: String?, value: String?, in document: BoardDocument) throws {
        try edit(document) { doc, board, err, size in
            switch (reference, value) {
            case let (r?, v?):
                return r.withCString { rp in v.withCString { vp in
                    kp_edit_set_ref_value(doc, board, Int32(index), rp, vp, err, size)
                } }
            case let (r?, nil):
                return r.withCString { rp in kp_edit_set_ref_value(doc, board, Int32(index), rp, nil, err, size) }
            case let (nil, v?):
                return v.withCString { vp in kp_edit_set_ref_value(doc, board, Int32(index), nil, vp, err, size) }
            case (nil, nil):
                return 0
            }
        }
    }

    func setTrackWidth(_ item: PickedItem, width: Double, in document: BoardDocument) throws {
        var c = item.cItem
        try edit(document) { doc, board, err, size in
            kp_edit_set_track_width(doc, board, &c, width, err, size)
        }
    }

    func setVia(_ index: Int, size viaSize: Double, drill: Double, in document: BoardDocument) throws {
        try edit(document) { doc, board, err, size in
            kp_edit_set_via(doc, board, Int32(index), viaSize, drill, err, size)
        }
    }
}

// MARK: - Pencil routing

/// Owns a core route (kp_route) and frees it.
final class RouteHandle {
    let pointer: OpaquePointer

    init(_ pointer: OpaquePointer) {
        self.pointer = pointer
    }

    deinit {
        kp_route_free(pointer)
    }
}

/// Where and how tracks are drawn with the pencil.
struct RouteSettings: Equatable {
    var slot: Int
    var width: Double
    var viaSize: Double
    var viaDrill: Double
}

/// A clean track built from a pencil stroke, ready to show and to commit.
struct RoutePreview {
    let handle: RouteHandle
    let ok: Bool
    let blocked: Bool
    let shorts: Bool
    let tooClose: Bool
    let startAnchored: Bool
    let endAnchored: Bool
    let net: Int
    let length: Double
    let message: String
    /// x1, y1, x2, y2 per track (mm)
    let tracks: [SIMD4<Float>]
    /// x, y, diameter per via
    let vias: [SIMD3<Float>]
    /// x, y, 1 = short / 0 = too close
    let conflicts: [SIMD3<Float>]
    let anchors: [SIMD2<Float>]
    let width: Double

    var canCommit: Bool { ok && !blocked && !tracks.isEmpty }
}

// MARK: - Design rule check

struct DRCEntry: Identifiable {
    let id: Int
    let code: Int
    let key: String
    let title: String
    let isError: Bool
    let position: SIMD2<Double>
    let message: String
    let itemA: PickedItem?
    let itemB: PickedItem?
}

struct DRCReport {
    let entries: [DRCEntry]
    let errors: Int
    let warnings: Int
    /// 0 = KiCad defaults, 1 = board file, 2 = project file
    let rulesSource: Int
    let note: String
    let milliseconds: Double
}

// MARK: - Fabrication

struct FabFileEntry: Identifiable {
    let id: Int
    let name: String
    let kind: String
    let size: Int
}

struct FabPackage {
    let files: [FabFileEntry]
    let zip: Data
    let warnings: [String]
    let ok: Bool
}

extension LoadedBoard {
    /// Copper layer slots, front first.
    var copperSlots: [LayerSlot] {
        slots.filter { $0.kind == .copper && $0.parentSlot < 0 && kp_board_slot_layer(handle, Int32($0.id)) >= 0 }
    }

    private static func floats(_ pointer: UnsafePointer<Float>?, _ count: Int) -> [Float] {
        guard let pointer, count > 0 else { return [] }
        return Array(UnsafeBufferPointer(start: pointer, count: count))
    }

    /// Builds the clean track for a stroke (board millimetres). Main thread only.
    func buildRoute(points: [SIMD2<Double>], settings: RouteSettings, tolerance: Double, snapRadius: Double,
                    clearance: Double) -> RoutePreview? {
        guard points.count >= 2 else { return nil }

        var xy: [Float] = []
        xy.reserveCapacity(points.count * 2)

        for p in points {
            xy.append(Float(p.x))
            xy.append(Float(p.y))
        }

        let pointer = xy.withUnsafeBufferPointer { buf -> OpaquePointer? in
            kp_route_build(handle, buf.baseAddress, Int32(points.count), Int32(settings.slot), settings.width,
                           tolerance, snapRadius, settings.viaSize, settings.viaDrill, clearance)
        }

        guard let pointer else { return nil }

        let owner = RouteHandle(pointer)
        var info = kp_route_info()
        kp_route_get_info(pointer, &info)

        var tc = 0, vc = 0, cc = 0, ac = 0
        let tp = kp_route_tracks(pointer, &tc)
        let vp = kp_route_vias(pointer, &vc)
        let cp = kp_route_conflicts(pointer, &cc)
        let ap = kp_route_anchors(pointer, &ac)
        let t = LoadedBoard.floats(tp, tc)
        let v = LoadedBoard.floats(vp, vc)
        let c = LoadedBoard.floats(cp, cc)
        let a = LoadedBoard.floats(ap, ac)

        var tracks: [SIMD4<Float>] = []
        var vias: [SIMD3<Float>] = []
        var conflicts: [SIMD3<Float>] = []
        var anchors: [SIMD2<Float>] = []

        for i in stride(from: 0, to: t.count - 3, by: 4) {
            tracks.append(SIMD4(t[i], t[i + 1], t[i + 2], t[i + 3]))
        }

        for i in stride(from: 0, to: v.count - 2, by: 3) {
            vias.append(SIMD3(v[i], v[i + 1], v[i + 2]))
        }

        for i in stride(from: 0, to: c.count - 2, by: 3) {
            conflicts.append(SIMD3(c[i], c[i + 1], c[i + 2]))
        }

        for i in stride(from: 0, to: a.count - 1, by: 2) {
            anchors.append(SIMD2(a[i], a[i + 1]))
        }

        return RoutePreview(handle: owner,
                            ok: info.ok != 0,
                            blocked: info.blocked != 0,
                            shorts: info.shorts != 0,
                            tooClose: info.too_close != 0,
                            startAnchored: info.start_kind != 0,
                            endAnchored: info.end_kind != 0,
                            net: Int(info.net),
                            length: info.length,
                            message: String(cString: kp_route_message(pointer)),
                            tracks: tracks,
                            vias: vias,
                            conflicts: conflicts,
                            anchors: anchors,
                            width: settings.width)
    }

    func commitRoute(_ route: RoutePreview, in document: BoardDocument) throws {
        try edit(document) { doc, board, err, size in
            kp_edit_add_route(doc, board, route.handle.pointer, err, size)
        }
    }

    /// Runs the design rule check (any thread; the board must stay alive meanwhile).
    func runDRC(project: Data?) -> DRCReport {
        let drc: OpaquePointer?

        if let project, !project.isEmpty {
            drc = project.withUnsafeBytes { raw -> OpaquePointer? in
                kp_board_drc(handle, raw.baseAddress?.assumingMemoryBound(to: CChar.self), raw.count)
            }
        } else {
            drc = kp_board_drc(handle, nil, 0)
        }

        guard let drc else {
            return DRCReport(entries: [], errors: 0, warnings: 0, rulesSource: 0, note: "", milliseconds: 0)
        }

        defer { kp_drc_free(drc) }

        var entries: [DRCEntry] = []
        let count = Int(kp_drc_count(drc))
        entries.reserveCapacity(count)

        for i in 0..<count {
            var it = kp_drc_item()
            kp_drc_get(drc, Int32(i), &it)
            let a = it.a.type != 0 ? PickedItem(it.a) : nil
            let b = it.b.type != 0 ? PickedItem(it.b) : nil
            entries.append(DRCEntry(id: i,
                                    code: Int(it.code),
                                    key: String(cString: kp_drc_key(it.code)),
                                    title: String(cString: kp_drc_title(it.code)),
                                    isError: it.severity == 0,
                                    position: SIMD2(it.x, it.y),
                                    message: String(cString: kp_drc_message(drc, Int32(i))),
                                    itemA: a,
                                    itemB: b))
        }

        return DRCReport(entries: entries,
                         errors: Int(kp_drc_error_count(drc)),
                         warnings: Int(kp_drc_warning_count(drc)),
                         rulesSource: Int(kp_drc_rules_source(drc)),
                         note: String(cString: kp_drc_note(drc)),
                         milliseconds: kp_drc_ms(drc))
    }

    /// Gerbers, drill files, BOM and placement, plus a zip of them (any thread).
    func makeFabrication(baseName: String, useAuxOrigin: Bool) -> FabPackage {
        let formatter = ISO8601DateFormatter()
        formatter.timeZone = TimeZone.current
        formatter.formatOptions = [.withInternetDateTime]
        let date = formatter.string(from: Date())

        let fab = baseName.withCString { b in
            date.withCString { d in
                kp_board_fab(handle, b, d, useAuxOrigin ? 1 : 0)
            }
        }

        guard let fab else {
            return FabPackage(files: [], zip: Data(), warnings: [], ok: false)
        }

        defer { kp_fab_free(fab) }

        var files: [FabFileEntry] = []

        for i in 0..<Int(kp_fab_file_count(fab)) {
            var length = 0
            _ = kp_fab_file_data(fab, Int32(i), &length)
            files.append(FabFileEntry(id: i,
                                      name: String(cString: kp_fab_file_name(fab, Int32(i))),
                                      kind: String(cString: kp_fab_file_kind(fab, Int32(i))),
                                      size: length))
        }

        var warnings: [String] = []

        for i in 0..<Int(kp_fab_warning_count(fab)) {
            warnings.append(String(cString: kp_fab_warning(fab, Int32(i))))
        }

        var zipLength = 0
        var zip = Data()

        if let z = kp_fab_zip(fab, &zipLength), zipLength > 0 {
            zip = Data(bytes: z, count: zipLength)
        }

        return FabPackage(files: files, zip: zip, warnings: warnings, ok: kp_fab_ok(fab) != 0)
    }
}
