// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.

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

/// Short message over the board.
struct Toast: Identifiable, Equatable {
    let id: Int
    let text: String
    let isError: Bool
    /// Offer "Undo" next to the message.
    let offersUndo: Bool
}

/// Sheets of the edit mode.
enum EditSheet: String, Identifiable {
    case fields, trackWidth, via

    var id: String { rawValue }
}

/// Something that would throw away unsaved edits, waiting for the user's answer.
enum PendingAction {
    case open(URL)
    case openFiles([URL])
    case openSample(SampleBoard)
    case close
}

/// Grid steps offered for moving footprints (mm, 0 = off).
let gridSteps: [Double] = [0, 0.1, 0.25, 0.5, 1.0, 0.635, 1.27, 2.54]

func gridTitle(_ step: Double) -> String {
    if step <= 0 {
        return String(localized: "Grid off")
    }

    let mil = step / 0.0254

    if abs(mil - mil.rounded()) < 1e-6, [25.0, 50.0, 100.0].contains(mil.rounded()) {
        return String(format: "%g mm (%g mil)", step, mil.rounded())
    }

    return String(format: "%g mm", step)
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
            } else if editMode {
                editMode = false
            }
        }
    }
    @Published private(set) var measureA: SIMD2<Double>?
    @Published private(set) var measureB: SIMD2<Double>?
    @Published private(set) var measureSnapA = false
    @Published private(set) var measureSnapB = false

    @Published private(set) var focusRequest: FocusRequest?
    private var focusCounter = 0

    // editing
    @Published var editMode = false {
        didSet {
            if editMode && board?.isFabrication == true {
                editMode = false // Gerber boards are for viewing only
                return
            }

            if editMode && measureMode {
                measureMode = false
            }

            if editMode != oldValue {
                promoteSelectionForEditing()
            }

            if !editMode && drawTool {
                drawTool = false
            }
        }
    }
    @Published private(set) var docState = DocumentState()
    @Published private(set) var isRebuilding = false
    @Published private(set) var dragPreview: SIMD2<Double>?
    @Published var gridStep: Double = 1.27 {
        didSet { UserDefaults.standard.set(gridStep, forKey: "NetloomGridStep") }
    }
    @Published var activeSheet: EditSheet?
    @Published private(set) var toast: Toast?
    @Published var pendingAction: PendingAction?
    @Published var showExporter = false
    @Published private(set) var exportFile = KiCadBoardFile(data: Data())

    // pencil track drawing
    @Published var drawTool = false {
        didSet {
            guard drawTool != oldValue else { return }

            if drawTool && board?.isFabrication == true {
                drawTool = false
                return
            }

            if drawTool {
                if !editMode {
                    editMode = true
                }

                selection = nil
                chooseRouteLayer()
            }

            routeHint = nil
            demoStroke = nil
        }
    }
    @Published var routeSettings = RouteSettings(slot: -1, width: 0.25, viaSize: 0.6, viaDrill: 0.3)
    @Published var fingerDrawing: Bool = (UserDefaults.standard.object(forKey: "NetloomFingerDraw") as? Bool) ?? true {
        didSet { UserDefaults.standard.set(fingerDrawing, forKey: "NetloomFingerDraw") }
    }
    @Published private(set) var routeHint: String?
    @Published private(set) var routeHintIsProblem = false
    @Published private(set) var demoStroke: [SIMD2<Double>]?

    // design rule check
    @Published private(set) var drcReport: DRCReport?
    @Published private(set) var drcRunning = false
    @Published var showDRC = false
    @Published private(set) var drcFocusID: Int?
    @Published private(set) var projectRulesName: String?
    @Published var showRulesImporter = false
    private var projectRules: Data?
    private var drcBoard: ObjectIdentifier?

    // fabrication outputs
    @Published var showFab = false
    @Published private(set) var fabPackage: FabPackage?
    @Published private(set) var fabRunning = false
    @Published var fabUseAuxOrigin = false {
        didSet { if fabUseAuxOrigin != oldValue { fabPackage = nil } }
    }
    @Published var showFabExporter = false
    @Published private(set) var fabZipFile = ZipFile(data: Data())
    private var fabBoard: ObjectIdentifier?

    private var document: BoardDocument?
    private var fileURL: URL?
    private var toastCounter = 0
    private var rebuildGeneration = 0

    /// Set from launch arguments (used by the CI screenshot job).
    @Published var autoPanel: String?

    let samples = [
        SampleBoard(id: "pic_programmer", title: String(localized: "PIC programmer (2 layers)")),
        SampleBoard(id: "ecc83-pp", title: String(localized: "ECC83 tube amplifier (2 layers)"))
    ]

    private var launchHandled = false
    private var pendingFlip = false
    private var pendingPanel: String?
    private var pendingActions: [String] = []

    init() {
        CoreLocalization.install()

        let saved = UserDefaults.standard.double(forKey: "NetloomGridStep")

        if UserDefaults.standard.object(forKey: "NetloomGridStep") != nil, saved >= 0 {
            gridStep = saved
        }
    }

    var highlight: HighlightSpec? {
        guard let selection else { return nil }
        return HighlightSpec(item: selection.item,
                             wholeNet: (highlightWholeNet || selection.item.itemType == .nothing)
                                 && selection.item.net > 0)
    }

    /// The footprint edits apply to: the selected footprint, or the footprint of a selected pad.
    var editFootprint: Int? {
        guard let item = selection?.item else { return nil }

        if item.itemType == .footprint {
            return Int(item.index)
        }

        return item.footprint >= 0 ? Int(item.footprint) : nil
    }

    /// Plain-file boards (samples) can only be saved under a new name.
    var canSaveInPlace: Bool { fileURL != nil }

    var canEdit: Bool { document != nil && board != nil && !isRebuilding && !isLoading }

    var documentName: String { board?.name ?? "board" }
    /// The open board came from Gerber / drill files (no editing, checks or exports).
    var isFabricationView: Bool { board?.isFabrication == true }

    // MARK: - Launch arguments (CI screenshots)

    /// -NetloomOpenSample <id>  -NetloomFlip YES  -NetloomPanel info|layers|search
    /// -NetloomDemo select|net|measure|labels|edit|route|drc|fab|gerber
    func handleLaunchArguments() {
        guard !launchHandled else { return }

        launchHandled = true
        let defaults = UserDefaults.standard

        if let id = defaults.string(forKey: "NetloomOpenSample"),
           let sample = samples.first(where: { $0.id == id }) {
            pendingFlip = defaults.bool(forKey: "NetloomFlip")
            pendingPanel = defaults.string(forKey: "NetloomPanel")

            if let demo = defaults.string(forKey: "NetloomDemo") {
                pendingActions.append(demo)
            }

            openSample(sample)
        }
    }

    private func runDemo(_ demo: String) {
        guard let board else { return }

        // pick the footprint with the most pads as a stable, interesting target
        let pads = board.footprints.map { fp -> (Int, Int) in
            (fp.id, board.footprintPadCount(fp.id))
        }
        let target = pads.max { $0.1 < $1.1 }?.0 ?? 0

        switch demo {
        case "select":
            selectFootprint(target)

        case "net":
            if let busiest = board.nets.max(by: { a, b in
                board.netPadCount(a.id) < board.netPadCount(b.id)
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

        case "edit":
            // edit mode with a rotated and moved footprint: shows the edit bar, the modified
            // state, undo, and the airwires the edit left behind
            editMode = true
            selectFootprint(target)
            rotateSelection(by: 90)

        case "route":
            // a wobbly stroke from a pad of the busiest footprint, shown with its clean track
            drawTool = true

            if let pose = board.footprintPose(target) {
                let start = board.snap(pose.position, tolerance: 6, visible: visibilityBytes()) ?? pose.position

                func stroke(_ dx: Double, _ dy: Double) -> [SIMD2<Double>] {
                    (0...60).map { i in
                        let t = Double(i) / 60
                        return SIMD2(start.x + dx * t,
                                     start.y + (t < 0.5 ? 0 : dy * (t - 0.5) * 2) + 0.25 * sin(t * 19))
                    }
                }

                // the first direction that gives a track which can be placed
                let tries: [(Double, Double)] = [(14, 9), (-14, 9), (14, -9), (-14, -9), (10, 4), (-10, 4),
                                                 (10, -4), (-10, -4), (6, 3), (-6, 3), (6, -3), (-6, -3)]
                var chosen = stroke(14, 9)

                for (dx, dy) in tries {
                    let points = stroke(dx, dy)

                    if let preview = board.buildRoute(points: points, settings: routeSettings, tolerance: 0.3,
                                                      snapRadius: 1.0, clearance: 0.2),
                       preview.canCommit {
                        chosen = points
                        break
                    }
                }

                demoStroke = chosen

                let xs = chosen.map(\.x), ys = chosen.map(\.y)
                let b = BoardBounds(minX: (xs.min() ?? start.x) - 6, minY: (ys.min() ?? start.y) - 6,
                                    maxX: (xs.max() ?? start.x) + 6, maxY: (ys.max() ?? start.y) + 6)
                focus(on: b, minSize: 20)
            }

        case "drc":
            runDRC()
            showDRC = true

        case "fab":
            showFab = true
            makeFabrication()

        case "gerber":
            // the board's own fabrication files, opened again as a Gerber set
            let zip = board.makeFabrication(baseName: board.name, useAuxOrigin: false).zip
            let name = board.name
            loadFabrication(name: name) { [(name: "\(name)-fab.zip", data: zip)] }

        default:
            break
        }
    }

    // MARK: - Opening

    func open(url: URL) {
        if docState.modified {
            pendingAction = .open(url)
            return
        }

        if url.hasDirectoryPath {
            openFabrication(urls: [url])
            return
        }

        let name = url.deletingPathExtension().lastPathComponent
        load(name: name, url: url) {
            try FileReader.read(url: url)
        }
    }

    /// Several files picked at once: a set of Gerber / drill files (or folders and zips of them).
    func open(urls: [URL]) {
        if urls.count == 1, let url = urls.first {
            open(url: url)
            return
        }

        guard !urls.isEmpty else { return }

        if docState.modified {
            pendingAction = .openFiles(urls)
            return
        }

        openFabrication(urls: urls)
    }

    private func openFabrication(urls: [URL]) {
        loadFabrication(name: Self.fabricationName(urls)) {
            try FileReader.readAll(urls: urls)
        }
    }

    private func loadFabrication(name: String, read: @escaping @Sendable () throws -> [(name: String, data: Data)]) {
        guard !isLoading else { return }

        isLoading = true
        loadingName = name
        errorMessage = nil

        Task.detached(priority: .userInitiated) {
            let outcome: Result<(BoardDocument?, LoadedBoard), Error>

            do {
                let loaded = try LoadedBoard.loadFabrication(files: try read(), name: name)
                outcome = .success((nil, loaded))
            } catch {
                outcome = .failure(error)
            }

            await self.finish(outcome, url: nil)
        }
    }

    /// "board" for board-F_Cu.gbr, board-B_Cu.gbr ...; a folder or zip gives its own name.
    nonisolated static func fabricationName(_ urls: [URL]) -> String {
        if urls.count == 1, let url = urls.first {
            return url.deletingPathExtension().lastPathComponent
        }

        let stems = urls.map { $0.deletingPathExtension().lastPathComponent }
        var prefix = stems.first ?? ""

        for stem in stems.dropFirst() {
            while !prefix.isEmpty && !stem.hasPrefix(prefix) {
                prefix.removeLast()
            }
        }

        let trimmed = prefix.trimmingCharacters(in: CharacterSet(charactersIn: "-_. "))
        return trimmed.isEmpty ? String(localized: "Gerber files") : trimmed
    }

    func openSample(_ sample: SampleBoard) {
        if docState.modified {
            pendingAction = .openSample(sample)
            return
        }

        guard let url = Bundle.main.url(forResource: sample.id, withExtension: "kicad_pcb") else {
            errorMessage = String(localized: "The sample file is missing from the app.")
            return
        }

        load(name: sample.id, url: nil) {
            try Data(contentsOf: url)
        }
    }

    private func load(name: String, url: URL?, read: @escaping @Sendable () throws -> Data) {
        guard !isLoading else { return }

        isLoading = true
        loadingName = name
        errorMessage = nil

        Task.detached(priority: .userInitiated) {
            let outcome: Result<(BoardDocument?, LoadedBoard), Error>

            do {
                let data = try read()

                if LoadedBoard.looksLikeFabrication(data) {
                    // a Gerber, drill or zip file: view only
                    let file = url?.lastPathComponent ?? name
                    let loaded = try LoadedBoard.loadFabrication(files: [(name: file, data: data)], name: name)
                    outcome = .success((nil, loaded))
                } else {
                    let doc = try BoardDocument.create(data: data)
                    let loaded = try LoadedBoard.load(document: doc, name: name, fileSize: data.count)
                    outcome = .success((doc, loaded))
                }
            } catch {
                outcome = .failure(error)
            }

            await self.finish(outcome, url: url)
        }
    }

    private func finish(_ outcome: Result<(BoardDocument?, LoadedBoard), Error>, url: URL?) {
        isLoading = false

        switch outcome {
        case .success(let (doc, loaded)):
            rebuildGeneration += 1 // a rebuild of the previous document must not land here
            document = doc
            fileURL = doc == nil ? nil : url
            docState = doc.map { DocumentState($0) } ?? DocumentState()
            dragPreview = nil
            isRebuilding = false
            activeSheet = nil
            board = loaded
            visibleSlots = Set(loaded.slots.filter { $0.defaultVisible }.map { $0.id })
            hiddenKinds = []
            flipped = pendingFlip
            pendingFlip = false
            cursorText = nil
            selection = nil
            highlightWholeNet = false
            measureMode = false
            editMode = false
            routeSettings.slot = -1
            drcReport = nil
            drcFocusID = nil
            showDRC = false
            fabPackage = nil
            showFab = false
            drawTool = false

            if doc != nil {
                loadSiblingRules(for: url)
            }

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
        if docState.modified {
            pendingAction = .close
            return
        }

        rebuildGeneration += 1
        board = nil
        document = nil
        fileURL = nil
        docState = DocumentState()
        cursorText = nil
        selection = nil
        editMode = false
        drcReport = nil
        fabPackage = nil
        projectRules = nil
        projectRulesName = nil
    }

    /// The user chose to drop the unsaved edits.
    func discardAndContinue() {
        guard let action = pendingAction else { return }

        pendingAction = nil
        docState.modified = false // lets the action through; the document is replaced anyway

        switch action {
        case .open(let url): open(url: url)
        case .openFiles(let urls): open(urls: urls)
        case .openSample(let sample): openSample(sample)
        case .close: close()
        }
    }

    /// Save in place, then carry on with the waiting action.
    func saveAndContinue() {
        save()

        if !docState.modified {
            discardAndContinue()
        } else {
            pendingAction = nil
        }
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

        var hits = board.pick(at: point, tolerance: tolerance, visible: visibilityBytes(),
                              hiddenKinds: hiddenKindMask(), flipped: flipped)

        if editMode {
            hits = editCandidates(hits)
        }

        guard let first = hits.first else {
            selection = nil
            return
        }

        select(first, candidates: hits, tapPoint: point)
    }

    /// In edit mode a pad or footprint text stands for its footprint: that is what can be
    /// moved, rotated or deleted.
    private func editCandidates(_ hits: [PickedItem]) -> [PickedItem] {
        var out: [PickedItem] = []

        for h in hits {
            let item = h.footprint >= 0 && h.itemType != .footprint ? PickedItem.footprint(h.footprint) : h

            if !out.contains(where: { $0.type == item.type && $0.index == item.index }) {
                out.append(item)
            }
        }

        return out
    }

    private func promoteSelectionForEditing() {
        guard editMode, let board, let sel = selection, sel.item.itemType != .footprint,
              sel.item.footprint >= 0 else { return }

        let item = PickedItem.footprint(sel.item.footprint)
        selection = Selection(item: item, info: board.describe(item), netInfo: nil,
                              candidates: editCandidates(sel.candidates), candidateIndex: 0,
                              tapPoint: sel.tapPoint)
        highlightWholeNet = false
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

    // MARK: - Editing

    /// What to select once the edited board has been rebuilt.
    private enum Reselect {
        case none
        /// Same item index (edits that keep the item order: move, rotate, fields, sizes).
        case item(PickedItem)
        /// A footprint found again by its reference (undo/redo may shift indices).
        case footprint(reference: String, index: Int)
    }

    private func showToast(_ text: String, isError: Bool = false, offersUndo: Bool = false) {
        toastCounter += 1
        let id = toastCounter
        toast = Toast(id: id, text: text, isError: isError, offersUndo: offersUndo)

        Task { [weak self] in
            try? await Task.sleep(nanoseconds: isError ? 5_000_000_000 : 3_000_000_000)

            if self?.toast?.id == id {
                self?.toast = nil
            }
        }
    }

    func dismissToast() {
        toast = nil
    }

    /// Runs one edit against the current board, then rebuilds it in the background.
    @discardableResult
    private func perform(_ reselect: Reselect, _ op: (LoadedBoard, BoardDocument) throws -> Void) -> Bool {
        guard let board, let document, !isRebuilding else { return false }

        do {
            try op(board, document)
        } catch {
            showToast(error.localizedDescription, isError: true)
            return false
        }

        rebuild(reselect)
        return true
    }

    private func rebuild(_ reselect: Reselect, retryAfterFailure: Bool = true) {
        guard let document, let board else { return }

        docState = DocumentState(document)
        isRebuilding = true
        rebuildGeneration += 1

        let generation = rebuildGeneration
        let name = board.name
        let size = board.fileSize

        Task.detached(priority: .userInitiated) {
            let outcome: Result<LoadedBoard, Error>

            do {
                outcome = .success(try LoadedBoard.load(document: document, name: name, fileSize: size))
            } catch {
                outcome = .failure(error)
            }

            await self.finishRebuild(outcome, generation: generation, reselect: reselect,
                                     retryAfterFailure: retryAfterFailure)
        }
    }

    private func finishRebuild(_ outcome: Result<LoadedBoard, Error>, generation: Int, reselect: Reselect,
                               retryAfterFailure: Bool) {
        guard generation == rebuildGeneration else { return }

        isRebuilding = false
        dragPreview = nil

        switch outcome {
        case .success(let loaded):
            let slotCount = loaded.slots.count
            board = loaded
            visibleSlots = visibleSlots.filter { $0 < slotCount }
            restoreSelection(reselect, on: loaded)

        case .failure(let error):
            // keep the old board on screen and take the edit back
            showToast(String(localized: "The board could not be updated: \(error.localizedDescription)"), isError: true)

            if retryAfterFailure, let document, document.undo() {
                rebuild(.none, retryAfterFailure: false)
            }
        }
    }

    private func restoreSelection(_ reselect: Reselect, on loaded: LoadedBoard) {
        var item: PickedItem?

        switch reselect {
        case .none:
            break

        case .item(let it):
            item = it

        case .footprint(let reference, let index):
            let matches = loaded.footprints.filter { $0.reference == reference }

            if matches.count == 1 {
                item = .footprint(Int32(matches[0].id))
            } else if index < loaded.footprints.count {
                item = .footprint(Int32(index))
            }
        }

        guard let item else {
            selection = nil
            return
        }

        let info = loaded.describe(item)

        guard !info.title.isEmpty || !info.rows.isEmpty else {
            selection = nil
            return
        }

        selection = Selection(item: item, info: info,
                              netInfo: item.net > 0 && item.itemType != .nothing ? loaded.describeNet(item.net) : nil,
                              candidates: [item], candidateIndex: 0, tapPoint: nil)
    }

    private var currentReselect: Reselect {
        guard let sel = selection else { return .none }
        return .item(sel.item)
    }

    func rotateSelection(by degrees: Double) {
        guard let fp = editFootprint else { return }

        perform(.item(.footprint(Int32(fp)))) { board, doc in
            try board.rotateFootprint(fp, degrees: degrees, in: doc)
        }
    }

    /// What "Delete" would remove, as shown on the button / confirmation.
    var deleteTarget: (item: PickedItem, title: String)? {
        guard let board, let sel = selection else { return nil }

        if let fp = editFootprint {
            let ref = fp < board.footprints.count ? board.footprints[fp].reference : ""
            return (.footprint(Int32(fp)), ref.isEmpty ? String(localized: "Footprint") : ref)
        }

        switch sel.item.itemType {
        case .track, .arc: return (sel.item, String(localized: "Track"))
        case .via: return (sel.item, String(localized: "Via"))
        case .zone: return (sel.item, String(localized: "Zone"))
        case .shape: return (sel.item, String(localized: "Graphic"))
        case .text: return (sel.item, String(localized: "Text"))
        default: return nil
        }
    }

    func deleteSelection() {
        guard let target = deleteTarget else { return }

        if perform(.none, { board, doc in try board.delete(target.item, in: doc) }) {
            selection = nil
            showToast(String(localized: "\(target.title) deleted"), offersUndo: true)
        }
    }

    func setFields(reference: String, value: String) {
        guard let fp = editFootprint, let board, fp < board.footprints.count else { return }

        let entry = board.footprints[fp]
        let newRef = reference != entry.reference ? reference : nil
        let newValue = value != entry.value ? value : nil

        guard newRef != nil || newValue != nil else { return }

        perform(.item(.footprint(Int32(fp)))) { board, doc in
            try board.setReferenceAndValue(fp, reference: newRef, value: newValue, in: doc)
        }
    }

    func setTrackWidth(_ width: Double) {
        guard let sel = selection, sel.item.itemType == .track || sel.item.itemType == .arc else { return }

        perform(currentReselect) { board, doc in
            try board.setTrackWidth(sel.item, width: width, in: doc)
        }
    }

    func setVia(size: Double, drill: Double) {
        guard let sel = selection, sel.item.itemType == .via else { return }

        perform(currentReselect) { board, doc in
            try board.setVia(Int(sel.item.index), size: size, drill: drill, in: doc)
        }
    }

    private var undoReselect: Reselect {
        guard let board, let fp = editFootprint, fp < board.footprints.count else { return .none }
        return .footprint(reference: board.footprints[fp].reference, index: fp)
    }

    func undo() {
        guard canEdit, let document else { return }

        let label = docState.undoLabel
        let reselect = undoReselect

        guard document.undo() else { return }

        rebuild(reselect)

        if !label.isEmpty {
            showToast(String(localized: "Undone: \(label)"))
        }
    }

    func redo() {
        guard canEdit, let document else { return }

        let label = docState.redoLabel
        let reselect = undoReselect

        guard document.redo() else { return }

        rebuild(reselect)

        if !label.isEmpty {
            showToast(String(localized: "Redone: \(label)"))
        }
    }

    // MARK: - Dragging footprints

    func dragCanBegin(at point: SIMD2<Double>, tolerance: Double) -> Bool {
        guard editMode, !drawTool, canEdit, let board, let fp = editFootprint else { return false }

        let hits = board.pick(at: point, tolerance: tolerance, visible: visibilityBytes(),
                              hiddenKinds: hiddenKindMask(), flipped: flipped)

        return hits.contains { Int($0.footprint) == fp || ($0.itemType == .footprint && Int($0.index) == fp) }
    }

    /// Offset that puts the footprint anchor on the grid.
    func dragSnapped(_ raw: SIMD2<Double>) -> SIMD2<Double> {
        guard gridStep > 0, let board, let fp = editFootprint, let pose = board.footprintPose(fp) else {
            return raw
        }

        let target = pose.position + raw
        let snapped = (target / gridStep).rounded(.toNearestOrAwayFromZero) * gridStep
        return snapped - pose.position
    }

    func dragEnded(_ raw: SIMD2<Double>) {
        guard let fp = editFootprint else { return }

        let delta = dragSnapped(raw)

        guard simd_length(delta) > 1e-6 else { return }

        if perform(.item(.footprint(Int32(fp))), { board, doc in try board.moveFootprint(fp, by: delta, in: doc) }) {
            dragPreview = delta // keeps the footprint where it was dropped until the rebuild is done
        }
    }

    // MARK: - Drawing tracks with the pencil

    /// Copper layer slots of the current board, front first.
    var copperSlots: [LayerSlot] { board?.copperSlots ?? [] }

    var routeToolConfig: RouteToolConfig? {
        guard drawTool, let board, routeSettings.slot >= 0, routeSettings.slot < board.slots.count else { return nil }

        return RouteToolConfig(settings: routeSettings, color: board.slots[routeSettings.slot].color,
                               allowFinger: fingerDrawing)
    }

    /// Draw on the side being looked at, unless a copper layer is already chosen.
    private func chooseRouteLayer() {
        let copper = copperSlots

        guard !copper.isEmpty else { return }

        if !copper.contains(where: { $0.id == routeSettings.slot }) {
            routeSettings.slot = (flipped ? copper.last : copper.first)?.id ?? -1
        }
    }

    func buildRoute(_ points: [SIMD2<Double>], tolerance: Double, snapRadius: Double) -> RoutePreview? {
        guard drawTool, let board, !isRebuilding, routeSettings.slot >= 0 else { return nil }

        let preview = board.buildRoute(points: points, settings: routeSettings, tolerance: tolerance,
                                       snapRadius: snapRadius, clearance: 0.2)
        updateRouteHint(preview)
        return preview
    }

    private func updateRouteHint(_ p: RoutePreview?) {
        var text: String?
        var problem = false

        if let p {
            if !p.message.isEmpty {
                text = p.message
                problem = p.blocked
            } else if p.tooClose {
                text = String(localized: "Closer than 0.2 mm to other copper")
                problem = true
            } else if p.ok {
                let net = p.net > 0 ? board?.nets.first(where: { $0.id == p.net })?.name : nil
                let length = String(format: "%.2f mm", p.length)
                text = net.map { String(localized: "\($0) · \(length)") } ?? length
            }
        }

        if text != routeHint || problem != routeHintIsProblem {
            routeHint = text
            routeHintIsProblem = problem
        }
    }

    func finishRoute(_ route: RoutePreview?) {
        guard let route else {
            routeHint = nil
            return
        }

        guard route.canCommit else {
            if !route.message.isEmpty {
                showToast(route.message, isError: route.blocked)
            }

            return
        }

        let note = route.message

        if perform(.none, { board, doc in try board.commitRoute(route, in: doc) }), !note.isEmpty {
            showToast(note)
        }
    }

    /// The first Apple Pencil touch switches finger drawing off: fingers move the board.
    func pencilSeen() {
        guard !UserDefaults.standard.bool(forKey: "NetloomPencilSeen") else { return }

        UserDefaults.standard.set(true, forKey: "NetloomPencilSeen")

        if fingerDrawing {
            fingerDrawing = false
            showToast(String(localized: "Apple Pencil draws; fingers move the board"))
        }
    }

    // MARK: - Design rule check

    var drcStale: Bool {
        guard drcReport != nil, let board else { return false }
        return drcBoard != ObjectIdentifier(board)
    }

    var drcMarkers: [DRCMarker] {
        guard let report = drcReport, !drcStale else { return [] }
        return report.entries.map { DRCMarker(position: $0.position, isError: $0.isError) }
    }

    var drcFocusPosition: SIMD2<Double>? {
        guard let id = drcFocusID, let report = drcReport, !drcStale, id < report.entries.count else { return nil }
        return report.entries[id].position
    }

    func runDRC() {
        guard let board, !drcRunning else { return }

        drcRunning = true
        drcFocusID = nil
        let rules = projectRules

        Task.detached(priority: .userInitiated) {
            let report = board.runDRC(project: rules)
            await self.finishDRC(report, board: board)
        }
    }

    private func finishDRC(_ report: DRCReport, board checked: LoadedBoard) {
        drcRunning = false
        drcReport = report
        drcBoard = ObjectIdentifier(checked)
    }

    func focusDRC(_ entry: DRCEntry) {
        drcFocusID = entry.id

        let r = 2.0
        focus(on: BoardBounds(minX: entry.position.x - r, minY: entry.position.y - r,
                              maxX: entry.position.x + r, maxY: entry.position.y + r), minSize: 4)

        if let board, let item = entry.itemA {
            let info = board.describe(item)

            if !info.title.isEmpty || !info.rows.isEmpty {
                selection = Selection(item: item, info: info,
                                      netInfo: item.net > 0 ? board.describeNet(item.net) : nil,
                                      candidates: [entry.itemA, entry.itemB].compactMap { $0 }, candidateIndex: 0,
                                      tapPoint: nil)
            }
        }
    }

    func clearDRC() {
        drcReport = nil
        drcFocusID = nil
    }

    /// Reads the KiCad project file next to the board, when the system lets us.
    private func loadSiblingRules(for url: URL?) {
        projectRules = nil
        projectRulesName = nil

        guard let url else { return }

        let pro = url.deletingPathExtension().appendingPathExtension("kicad_pro")

        if let data = try? FileReader.read(url: pro), !data.isEmpty {
            projectRules = data
            projectRulesName = pro.lastPathComponent
        }
    }

    func importRules(_ result: Result<[URL], Error>) {
        guard case .success(let urls) = result, let url = urls.first else { return }

        do {
            projectRules = try FileReader.read(url: url)
            projectRulesName = url.lastPathComponent
            runDRC()
        } catch {
            showToast(error.localizedDescription, isError: true)
        }
    }

    // MARK: - Fabrication outputs

    var fabStale: Bool {
        guard fabPackage != nil, let board else { return false }
        return fabBoard != ObjectIdentifier(board)
    }

    func makeFabrication() {
        guard let board, !fabRunning else { return }

        fabRunning = true
        let name = documentName
        let aux = fabUseAuxOrigin

        Task.detached(priority: .userInitiated) {
            let package = board.makeFabrication(baseName: name, useAuxOrigin: aux)
            await self.finishFabrication(package, board: board)
        }
    }

    private func finishFabrication(_ package: FabPackage, board made: LoadedBoard) {
        fabRunning = false
        fabPackage = package
        fabBoard = ObjectIdentifier(made)
    }

    func exportFabrication() {
        guard let package = fabPackage, !package.zip.isEmpty else { return }

        fabZipFile = ZipFile(data: package.zip)
        showFabExporter = true
    }

    func fabExportFinished(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            showToast(String(localized: "Saved \(url.lastPathComponent)"))
        case .failure(let error):
            if (error as NSError).code != NSUserCancelledError {
                showToast(error.localizedDescription, isError: true)
            }
        }
    }

    // MARK: - Saving

    func save() {
        guard let document, !isRebuilding else { return }

        guard let url = fileURL else {
            saveAs()
            return
        }

        do {
            try FileWriter.write(document.text(), to: url)
            document.markSaved()
            docState = DocumentState(document)
            showToast(String(localized: "Saved: \(url.lastPathComponent)"))
        } catch {
            showToast(String(localized: "Could not save: \(error.localizedDescription). Use \"Save as\" to save it somewhere else."),
                      isError: true)
        }
    }

    func saveAs() {
        guard let document, !isRebuilding else { return }

        exportFile = KiCadBoardFile(data: document.text())
        showExporter = true
    }

    func exportFinished(_ result: Result<URL, Error>) {
        switch result {
        case .success(let url):
            document?.markSaved()

            if let document {
                docState = DocumentState(document)
            }

            fileURL = url
            showToast(String(localized: "Saved: \(url.lastPathComponent)"))

        case .failure(let error):
            showToast(String(localized: "Could not save: \(error.localizedDescription)"), isError: true)
        }
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
        var result: Result<Data, Error> = .failure(NetloomError.message(String(localized: "The file could not be read.")))

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

    /// The files behind a picked selection: files as they are, folders one level deep.
    static func readAll(urls: [URL]) throws -> [(name: String, data: Data)] {
        var files: [(name: String, data: Data)] = []
        var total = 0
        let limit = 512 * 1024 * 1024

        for url in urls {
            if url.hasDirectoryPath {
                let scoped = url.startAccessingSecurityScopedResource()
                defer {
                    if scoped {
                        url.stopAccessingSecurityScopedResource()
                    }
                }

                let entries = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isRegularFileKey],
                                                                          options: [.skipsHiddenFiles])

                for entry in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                    guard (try? entry.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true else { continue }

                    let data = try Data(contentsOf: entry, options: [.mappedIfSafe])
                    total += data.count
                    files.append((name: entry.lastPathComponent, data: data))
                }
            } else {
                let data = try read(url: url)
                total += data.count
                files.append((name: url.lastPathComponent, data: data))
            }

            if total > limit {
                throw NetloomError.message(String(localized: "The selected files are too large."))
            }
        }

        if files.isEmpty {
            throw NetloomError.message(String(localized: "The folder is empty."))
        }

        return files
    }
}
