// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import Foundation
import SwiftUI
import UniformTypeIdentifiers

extension UTType {
    /// Declared in Info.plist (UTImportedTypeDeclarations).
    static let kicadPCB = UTType(importedAs: "org.kicad.pcb")
}

struct SampleBoard: Identifiable {
    let id: String      // resource name in the bundle
    let title: String
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

    /// Set from launch arguments (used by the CI screenshot job).
    @Published var autoPanel: String?

    let samples = [
        SampleBoard(id: "pic_programmer", title: "PIC programlayıcı (2 katman)"),
        SampleBoard(id: "ecc83-pp", title: "ECC83 tüp amfi (2 katman)")
    ]

    private var launchHandled = false
    private var pendingFlip = false
    private var pendingPanel: String?

    /// Launch arguments: -KiPadOpenSample <id>  -KiPadFlip YES  -KiPadPanel info|layers
    func handleLaunchArguments() {
        guard !launchHandled else { return }

        launchHandled = true
        let defaults = UserDefaults.standard

        if let id = defaults.string(forKey: "KiPadOpenSample"),
           let sample = samples.first(where: { $0.id == id }) {
            pendingFlip = defaults.bool(forKey: "KiPadFlip")
            pendingPanel = defaults.string(forKey: "KiPadPanel")
            openSample(sample)
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
            fitToken += 1

            if let panel = pendingPanel {
                pendingPanel = nil
                autoPanel = panel
            }

        case .failure(let error):
            errorMessage = error.localizedDescription
        }
    }

    func close() {
        board = nil
        cursorText = nil
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
