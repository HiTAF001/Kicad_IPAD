// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import Foundation
import SwiftUI
import UniformTypeIdentifiers
import simd

extension UTType {
    /// Declared in Info.plist (UTImportedTypeDeclarations).
    static let kicadPCB = UTType(importedAs: "org.kicad.pcb")
}

struct SampleBoard: Identifiable {
    let id: String      // resource name in the bundle
    let title: String
}

/// What is selected and what the user can cycle through at the same spot.
struct Selection: Equatable {
    var item: PickedItem
    var info: ItemDescription
    var netInfo: ItemDescription?
    var candidates: [PickedItem]
    var candidateIndex: Int
    var tapPoint: SIMD2<Double>?
}

/// What the renderer should light up.
struct HighlightSpec: Equatable {
    var item: PickedItem
    var wholeNet: Bool
}

/// Ask the renderer to frame a region.
struct FocusRequest: Equatable {
    var bounds: BoardBounds
    var id: Int
    /// Keep the right side free for the selection card.
    var reserveRight: Bool
}

@MainActor
final class BoardStore: ObservableObject {
    @Published private(set) var board: LoadedBoard?
    @Published private(set) var isLoading = false
    @Published private(set) var loadingName = ""
    @Published var errorMessage: String?
    @Published var visibleSlots: Set<Int> = []
    @Published var hiddenKinds: Set<Int> = []
    @Published var flipped = false
    @Published private(set) var fitToken = 0
    @Published var cursorText: String?

    // selection & highlight
    @Published private(set) var selection: Selection?
    @Published var highlightWholeNet = false

    // measuring
    @Published var measureMode = false {
        didSet {
            if !measureMode {
                measureA = nil
                measureB = nil
            }
        }
    }
    @Published private(set) var measureA: SIMD2<Double>?
    @Published private(set) var measureB: SIMD2<Double>?
    @Published private(set) var measureSnapA = false
    @Published private(set) var measureSnapB = false

    @Published private(set) var focusRequest: FocusRequest?
    private var focusCounter = 0

    /// Set from launch arguments (used by the CI screenshot job).
    @Published var autoPanel: String?

    let samples = [
        SampleBoard(id: "pic_programmer", title: "PIC programlayıcı (2 katman)"),
        SampleBoard(id: "ecc83-pp", title: "ECC83 tüp amfi (2 katman)")
    ]

    private var launchHandled = false
    private var pendingFlip = false
    private var pendingPanel: String?
    private var pendingActions: [String] = []

    var highlight: HighlightSpec? {
        guard let selection else { return nil }
        return HighlightSpec(item: selection.item,
                             wholeNet: (highlightWholeNet || selection.item.itemType == .nothing)
                                 && selection.item.net > 0)
    }

    // MARK: - Launch arguments (CI screenshots)

    /// -KiPadOpenSample <id>  -KiPadFlip YES  -KiPadPanel info|layers|search
    /// -KiPadDemo select|net|measure|labels
    func handleLaunchArguments() {
        guard !launchHandled else { return }

        launchHandled = true
        let defaults = UserDefaults.standard

        if let id = defaults.string(forKey: "KiPadOpenSample"),
           let sample = samples.first(where: { $0.id == id }) {
            pendingFlip = defaults.bool(forKey: "KiPadFlip")
            pendingPanel = defaults.string(forKey: "KiPadPanel")

            if let demo = defaults.string(forKey: "KiPadDemo") {
                pendingActions.append(demo)
            }

            openSample(sample)
        }
    }

    private func runDemo(_ demo: String) {
        guard let board else { return }

        // pick the footprint with the most pads as a stable, interesting target
        let pads = board.footprints.map { fp -> (Int, Int) in
            let n = Int(board.describe(.footprint(Int32(fp.id))).value(for: "Pad sayısı") ?? "0") ?? 0
            return (fp.id, n)
        }
        let target = pads.max { $0.1 < $1.1 }?.0 ?? 0

        switch demo {
        case "select":
            selectFootprint(target)

        case "net":
            if let busiest = board.nets.max(by: { a, b in
                (Int(board.describeNet(Int32(a.id)).value(for: "Pad") ?? "0") ?? 0)
                    < (Int(board.describeNet(Int32(b.id)).value(for: "Pad") ?? "0") ?? 0)
            }) {
                selectNet(busiest.id)
            }

        case "measure":
            measureMode = true
            let b = board.bounds
            measureA = SIMD2(b.minX + b.width * 0.2, b.minY + b.height * 0.3)
            measureB = SIMD2(b.minX + b.width * 0.7, b.minY + b.height * 0.7)

        case "labels":
            if let bounds = board.bounds(of: .footprint(0), wholeNet: false) {
                focus(on: bounds, minSize: 12)
            }

        default:
            break
        }
    }

    // MARK: - Opening

    func open(url: URL) {
        let name = url.deletingPathExtension().lastPathComponent
        load(name: name) {
            try FileReader.read(url: url)
        }
    }

    func openSample(_ sample: SampleBoard) {
        guard let url = Bundle.main.url(forResource: sample.id, withExtension: "kicad_pcb") else {
            errorMessage = "Örnek dosya uygulama paketinde bulunamadı."
            return
        }

        load(name: sample.id) {
            try Data(contentsOf: url)
        }
    }

    private func load(name: String, read: @escaping @Sendable () throws -> Data) {
        guard !isLoading else { return }

        isLoading = true
        loadingName = name
        errorMessage = nil

        Task.detached(priority: .userInitiated) {
            let outcome: Result<LoadedBoard, Error>

            do {
                let data = try read()
                outcome = .success(try LoadedBoard.load(data: data, name: name))
            } catch {
                outcome = .failure(error)
            }

            await self.finish(outcome)
        }
    }

    private func finish(_ outcome: Result<LoadedBoard, Error>) {
        isLoading = false

        switch outcome {
        case .success(let loaded):
            board = loaded
            visibleSlots = Set(loaded.slots.filter { $0.defaultVisible }.map { $0.id })
            hiddenKinds = []
            flipped = pendingFlip
            pendingFlip = false
            cursorText = nil
            selection = nil
            highlightWholeNet = false
            measureMode = false
            fitToken += 1

            if let panel = pendingPanel {
                pendingPanel = nil
                autoPanel = panel
            }

            let actions = pendingActions
            pendingActions = []

            for action in actions {
                runDemo(action)
            }

        case .failure(let error):
            errorMessage = error.localizedDescription
        }
    }

    func close() {
        board = nil
        cursorText = nil
        selection = nil
    }

    // MARK: - View state

    func requestFit() {
        fitToken += 1
    }

    func isVisible(_ slot: LayerSlot) -> Bool {
        visibleSlots.contains(slot.id)
    }

    func setVisible(_ slot: LayerSlot, _ visible: Bool) {
        if visible {
            visibleSlots.insert(slot.id)
        } else {
            visibleSlots.remove(slot.id)
        }
    }

    func isShown(_ category: ObjectCategory) -> Bool {
        !category.kinds.contains { hiddenKinds.contains($0) }
    }

    func setShown(_ category: ObjectCategory, _ shown: Bool) {
        for k in category.kinds {
            if shown {
                hiddenKinds.remove(k)
            } else {
                hiddenKinds.insert(k)
            }
        }
    }

    func showAllLayers() {
        guard let board else { return }
        visibleSlots = Set(board.slots.map { $0.id })
    }

    /// Only copper, board outline and holes: what you need to follow traces.
    func showCopperOnly() {
        guard let board else { return }
        visibleSlots = Set(board.slots.filter {
            $0.kind == .copper || $0.kind == .edge || $0.kind == .virtualSlot
        }.map { $0.id })
    }

    /// Only one side: its copper, silk, mask and the outline.
    func showSide(front: Bool) {
        guard let board else { return }
        let side = front ? 1 : -1
        visibleSlots = Set(board.slots.filter {
            $0.side == side || $0.kind == .edge || $0.kind == .virtualSlot
        }.map { $0.id })
        flipped = !front
    }

    func setCursor(_ point: SIMD2<Double>) {
        cursorText = String(format: "X %.3f mm   Y %.3f mm", point.x, point.y)
    }

    /// One byte per slot for the core's picking functions.
    private func visibilityBytes() -> [UInt8] {
        guard let board else { return [] }
        return board.slots.map { visibleSlots.contains($0.id) ? 1 : 0 }
    }

    private func hiddenKindMask() -> UInt32 {
        hiddenKinds.reduce(UInt32(0)) { mask, kind in
            kind >= 0 && kind < 32 ? mask | (UInt32(1) << UInt32(kind)) : mask
        }
    }

    // MARK: - Taps

    /// A tap on the board at a world position; tolerance is in millimetres.
    func handleTap(at point: SIMD2<Double>, tolerance: Double) {
        guard let board else { return }

        setCursor(point)

        if measureMode {
            measureTap(at: point, tolerance: tolerance)
            return
        }

        // tapping the same spot again walks through the items stacked there
        if var sel = selection, let tp = sel.tapPoint, simd_distance(tp, point) <= tolerance,
           sel.candidates.count > 1 {
            sel.candidateIndex = (sel.candidateIndex + 1) % sel.candidates.count
            sel.item = sel.candidates[sel.candidateIndex]
            sel.info = board.describe(sel.item)
            sel.netInfo = sel.item.net > 0 ? board.describeNet(sel.item.net) : nil
            selection = sel
            return
        }

        let hits = board.pick(at: point, tolerance: tolerance, visible: visibilityBytes(),
                              hiddenKinds: hiddenKindMask(), flipped: flipped)

        guard let first = hits.first else {
            selection = nil
            return
        }

        select(first, candidates: hits, tapPoint: point)
    }

    private func select(_ item: PickedItem, candidates: [PickedItem], tapPoint: SIMD2<Double>?) {
        guard let board else { return }

        selection = Selection(item: item,
                              info: board.describe(item),
                              netInfo: item.net > 0 && item.itemType != .nothing ? board.describeNet(item.net) : nil,
                              candidates: candidates,
                              candidateIndex: 0,
                              tapPoint: tapPoint)
    }

    func nextCandidate() {
        guard let board, var sel = selection, sel.candidates.count > 1 else { return }

        sel.candidateIndex = (sel.candidateIndex + 1) % sel.candidates.count
        sel.item = sel.candidates[sel.candidateIndex]
        sel.info = board.describe(sel.item)
        sel.netInfo = sel.item.net > 0 ? board.describeNet(sel.item.net) : nil
        selection = sel
    }

    func clearSelection() {
        selection = nil
        highlightWholeNet = false
    }

    func selectFootprint(_ index: Int) {
        let item = PickedItem.footprint(Int32(index))
        select(item, candidates: [item], tapPoint: nil)
        highlightWholeNet = false
        focusOnSelection()
    }

    func selectNet(_ net: Int) {
        let item = PickedItem.net(Int32(net))
        select(item, candidates: [item], tapPoint: nil)
        highlightWholeNet = true
        focusOnSelection()
    }

    func focusOnSelection() {
        guard let board, let sel = selection else { return }

        let wholeNet = sel.item.itemType == .nothing || (highlightWholeNet && sel.item.net > 0)

        if let b = board.bounds(of: sel.item, wholeNet: wholeNet) {
            focus(on: b, minSize: wholeNet ? 5 : 3, reserveRight: true)
        }
    }

    private func focus(on bounds: BoardBounds, minSize: Double, reserveRight: Bool = false) {
        var b = bounds
        let grow = max(0, minSize - b.width) / 2
        let growY = max(0, minSize - b.height) / 2
        b.minX -= grow
        b.maxX += grow
        b.minY -= growY
        b.maxY += growY
        focusCounter += 1
        focusRequest = FocusRequest(bounds: b, id: focusCounter, reserveRight: reserveRight)
    }

    // MARK: - Measuring

    private func measureTap(at point: SIMD2<Double>, tolerance: Double) {
        guard let board else { return }

        let snapped = board.snap(point, tolerance: tolerance, visible: visibilityBytes())
        let p = snapped ?? point

        if measureA == nil || measureB != nil {
            measureA = p
            measureSnapA = snapped != nil
            measureB = nil
            measureSnapB = false
        } else {
            measureB = p
            measureSnapB = snapped != nil
        }
    }

    func clearMeasurement() {
        measureA = nil
        measureB = nil
    }

    var measurementSummary: String? {
        guard let a = measureA, let b = measureB else { return nil }

        let d = b - a
        let dist = simd_length(d)
        // KiCad angle sense: counter-clockwise on screen (Y axis points down)
        var angle = atan2(-d.y, d.x) * 180 / .pi

        if angle < 0 {
            angle += 360
        }

        return String(format: "%.3f mm   ΔX %.3f   ΔY %.3f   %.1f°", dist, d.x, d.y, angle)
    }
}

/// Reads a document the user picked, coordinating with iCloud Drive / file providers.
enum FileReader {
    static func read(url: URL) throws -> Data {
        let scoped = url.startAccessingSecurityScopedResource()
        defer {
            if scoped {
                url.stopAccessingSecurityScopedResource()
            }
        }

        var coordinationError: NSError?
        var result: Result<Data, Error> = .failure(KiPadError.message("Dosya okunamadı."))

        NSFileCoordinator().coordinate(readingItemAt: url, options: [.withoutChanges],
                                       error: &coordinationError) { readURL in
            do {
                result = .success(try Data(contentsOf: readURL, options: [.mappedIfSafe]))
            } catch {
                result = .failure(error)
            }
        }

        if let coordinationError {
            throw coordinationError
        }

        return try result.get()
    }
}
