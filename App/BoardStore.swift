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

/// Short message over the board.
struct Toast: Identifiable, Equatable {
    let id: Int
    let text: String
    let isError: Bool
    /// Offer "Geri al" next to the message.
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
    case openSample(SampleBoard)
    case close
}

/// Grid steps offered for moving footprints (mm, 0 = off).
let gridSteps: [Double] = [0, 0.1, 0.25, 0.5, 1.0, 0.635, 1.27, 2.54]

func gridTitle(_ step: Double) -> String {
    if step <= 0 {
        return "Izgara kapalı"
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
            if editMode && measureMode {
                measureMode = false
            }

            if editMode != oldValue {
                promoteSelectionForEditing()
            }
        }
    }
    @Published private(set) var docState = DocumentState()
    @Published private(set) var isRebuilding = false
    @Published private(set) var dragPreview: SIMD2<Double>?
    @Published var gridStep: Double = 1.27 {
        didSet { UserDefaults.standard.set(gridStep, forKey: "KiPadGridStep") }
    }
    @Published var activeSheet: EditSheet?
    @Published private(set) var toast: Toast?
    @Published var pendingAction: PendingAction?
    @Published var showExporter = false
    @Published private(set) var exportFile = KiCadBoardFile(data: Data())

    private var document: BoardDocument?
    private var fileURL: URL?
    private var toastCounter = 0
    private var rebuildGeneration = 0

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

    init() {
        let saved = UserDefaults.standard.double(forKey: "KiPadGridStep")

        if UserDefaults.standard.object(forKey: "KiPadGridStep") != nil, saved >= 0 {
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

    var documentName: String { board?.name ?? "kart" }

    // MARK: - Launch arguments (CI screenshots)

    /// -KiPadOpenSample <id>  -KiPadFlip YES  -KiPadPanel info|layers|search
    /// -KiPadDemo select|net|measure|labels|edit
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

        case "edit":
            // edit mode with a rotated and moved footprint: shows the edit bar, the modified
            // state, undo, and the airwires the edit left behind
            editMode = true
            selectFootprint(target)
            rotateSelection(by: 90)

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

        let name = url.deletingPathExtension().lastPathComponent
        load(name: name, url: url) {
            try FileReader.read(url: url)
        }
    }

    func openSample(_ sample: SampleBoard) {
        if docState.modified {
            pendingAction = .openSample(sample)
            return
        }

        guard let url = Bundle.main.url(forResource: sample.id, withExtension: "kicad_pcb") else {
            errorMessage = "Örnek dosya uygulama paketinde bulunamadı."
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
            let outcome: Result<(BoardDocument, LoadedBoard), Error>

            do {
                let data = try read()
                let doc = try BoardDocument.create(data: data)
                let loaded = try LoadedBoard.load(document: doc, name: name, fileSize: data.count)
                outcome = .success((doc, loaded))
            } catch {
                outcome = .failure(error)
            }

            await self.finish(outcome, url: url)
        }
    }

    private func finish(_ outcome: Result<(BoardDocument, LoadedBoard), Error>, url: URL?) {
        isLoading = false

        switch outcome {
        case .success(let (doc, loaded)):
            rebuildGeneration += 1 // a rebuild of the previous document must not land here
            document = doc
            fileURL = url
            docState = DocumentState(doc)
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
    }

    /// The user chose to drop the unsaved edits.
    func discardAndContinue() {
        guard let action = pendingAction else { return }

        pendingAction = nil
        docState.modified = false // lets the action through; the document is replaced anyway

        switch action {
        case .open(let url): open(url: url)
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
            showToast("Kart güncellenemedi: \(error.localizedDescription)", isError: true)

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

    /// What "Sil" would remove, as shown on the button / confirmation.
    var deleteTarget: (item: PickedItem, title: String)? {
        guard let board, let sel = selection else { return nil }

        if let fp = editFootprint {
            let ref = fp < board.footprints.count ? board.footprints[fp].reference : ""
            return (.footprint(Int32(fp)), ref.isEmpty ? "Footprint" : ref)
        }

        switch sel.item.itemType {
        case .track, .arc: return (sel.item, "Pist")
        case .via: return (sel.item, "Via")
        case .zone: return (sel.item, "Bölge")
        case .shape: return (sel.item, "Çizim")
        case .text: return (sel.item, "Yazı")
        default: return nil
        }
    }

    func deleteSelection() {
        guard let target = deleteTarget else { return }

        if perform(.none, { board, doc in try board.delete(target.item, in: doc) }) {
            selection = nil
            showToast("\(target.title) silindi", offersUndo: true)
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
            showToast("Geri alındı: \(label)")
        }
    }

    func redo() {
        guard canEdit, let document else { return }

        let label = docState.redoLabel
        let reselect = undoReselect

        guard document.redo() else { return }

        rebuild(reselect)

        if !label.isEmpty {
            showToast("Yinelendi: \(label)")
        }
    }

    // MARK: - Dragging footprints

    func dragCanBegin(at point: SIMD2<Double>, tolerance: Double) -> Bool {
        guard editMode, canEdit, let board, let fp = editFootprint else { return false }

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
            showToast("Kaydedildi: \(url.lastPathComponent)")
        } catch {
            showToast("Kaydedilemedi: \(error.localizedDescription). \"Farklı kaydet\" ile başka yere kaydedebilirsin.",
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
            showToast("Kaydedildi: \(url.lastPathComponent)")

        case .failure(let error):
            showToast("Kaydedilemedi: \(error.localizedDescription)", isError: true)
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
