// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.
//
// Swift wrapper around the C core: loads a board, copies its mesh into Metal buffers and keeps
// the metadata the UI needs. Picking, descriptions and highlight ranges are forwarded to the
// core (call those from the main thread only).

import Foundation
import Metal
import simd

enum KiPadError: LocalizedError {
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
}

/// Object categories the user can toggle.
enum ObjectCategory: String, CaseIterable, Identifiable {
    case tracks, vias, pads, zones, graphics, texts, holes, labels

    var id: String { rawValue }

    var title: String {
        switch self {
        case .tracks: return "Pistler"
        case .vias: return "Via'lar"
        case .pads: return "Pad'ler"
        case .zones: return "Bakır bölgeler"
        case .graphics: return "Çizimler"
        case .texts: return "Yazılar"
        case .holes: return "Delikler"
        case .labels: return "Pad numarası ve net adları"
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
    var subtitle: String? { (display.isEmpty || display == name) ? nil : name }
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

    private let handle: OpaquePointer

    private init(name: String, fileSize: Int, handle: OpaquePointer, device: MTLDevice) throws {
        self.name = name
        self.fileSize = fileSize
        self.handle = handle

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
                throw KiPadError.message("Kart bu cihazın grafik belleği için çok büyük "
                    + "(\(vBytes / 1_048_576) MB köşe, \(iBytes / 1_048_576) MB indeks).")
            }

            vb = device.makeBuffer(bytes: vptr, length: vBytes, options: .storageModeShared)
            ib = device.makeBuffer(bytes: iptr, length: iBytes, options: .storageModeShared)

            if vb == nil || ib == nil {
                throw KiPadError.message("Grafik belleği ayrılamadı (kart çok büyük olabilir).")
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
            throw KiPadError.message("Bu cihaz Metal grafik arayüzünü desteklemiyor.")
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
            throw KiPadError.message(message.isEmpty ? "Dosya açılamadı." : message)
        }

        do {
            return try LoadedBoard(name: name, fileSize: data.count, handle: handle, device: device)
        } catch {
            // init threw before taking ownership through deinit
            kp_board_free(handle)
            throw error
        }
    }

    // MARK: - Queries (main thread)

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
}
