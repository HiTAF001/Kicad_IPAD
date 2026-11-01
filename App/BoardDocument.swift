// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.
//
// The editable file behind a board (kp_doc in the core). It keeps the original text and writes
// unedited items back byte for byte, with undo/redo.
//
// Not thread-safe. BoardStore guarantees that only one piece of code uses it at a time: edits,
// undo/redo and saving run on the main actor while no rebuild is running, and a rebuild (which
// reads the text on a background task) only starts after the main actor is done with it.

import Foundation
import SwiftUI
import UniformTypeIdentifiers

final class BoardDocument: @unchecked Sendable {
    let handle: OpaquePointer
    /// Same for every board built from this document (keeps the camera across rebuilds).
    let lineage: Int

    private static var nextLineage = 1
    private static let lineageLock = NSLock()

    private init(handle: OpaquePointer) {
        self.handle = handle
        BoardDocument.lineageLock.lock()
        lineage = BoardDocument.nextLineage
        BoardDocument.nextLineage += 1
        BoardDocument.lineageLock.unlock()
    }

    deinit {
        kp_doc_free(handle)
    }

    static func create(data: Data) throws -> BoardDocument {
        let errorSize = 1024
        var errorBuffer = [CChar](repeating: 0, count: errorSize)

        let handle: OpaquePointer? = data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> OpaquePointer? in
            guard let base = raw.baseAddress, raw.count > 0 else { return nil }
            return errorBuffer.withUnsafeMutableBufferPointer { errPtr -> OpaquePointer? in
                kp_doc_create(base.assumingMemoryBound(to: CChar.self), raw.count, errPtr.baseAddress, errorSize)
            }
        }

        guard let handle else {
            let message = errorBuffer.withUnsafeBufferPointer { ptr -> String in
                guard let base = ptr.baseAddress else { return "" }
                return String(cString: base)
            }
            throw KiPadError.message(message.isEmpty ? "Dosya açılamadı." : message)
        }

        return BoardDocument(handle: handle)
    }

    var isModified: Bool { kp_doc_modified(handle) != 0 }
    var canUndo: Bool { kp_doc_can_undo(handle) != 0 }
    var canRedo: Bool { kp_doc_can_redo(handle) != 0 }
    var undoLabel: String { kp_doc_undo_label(handle).map { String(cString: $0) } ?? "" }
    var redoLabel: String { kp_doc_redo_label(handle).map { String(cString: $0) } ?? "" }

    func undo() -> Bool { kp_doc_undo(handle) != 0 }
    func redo() -> Bool { kp_doc_redo(handle) != 0 }
    func markSaved() { kp_doc_mark_saved(handle) }

    /// The whole file as it would be saved now.
    func text() -> Data {
        var length = 0

        guard let p = kp_doc_text(handle, &length), length > 0 else { return Data() }

        return Data(bytes: p, count: length)
    }
}

/// Snapshot of the document state the UI shows (read on the main actor after each change).
struct DocumentState: Equatable {
    var modified = false
    var canUndo = false
    var canRedo = false
    var undoLabel = ""
    var redoLabel = ""

    init() {}

    init(_ doc: BoardDocument) {
        modified = doc.isModified
        canUndo = doc.canUndo
        canRedo = doc.canRedo
        undoLabel = canUndo ? doc.undoLabel : ""
        redoLabel = canRedo ? doc.redoLabel : ""
    }
}

/// File wrapper for "Farklı kaydet" (fileExporter).
struct KiCadBoardFile: FileDocument {
    static var readableContentTypes: [UTType] { [.kicadPCB] }
    static var writableContentTypes: [UTType] { [.kicadPCB] }

    var data: Data

    init(data: Data) {
        self.data = data
    }

    init(configuration: ReadConfiguration) throws {
        data = configuration.file.regularFileContents ?? Data()
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: data)
    }
}

/// Writes a board back to a document the user opened, coordinating with file providers.
enum FileWriter {
    static func write(_ data: Data, to url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer {
            if scoped {
                url.stopAccessingSecurityScopedResource()
            }
        }

        var coordinationError: NSError?
        var result: Result<Void, Error> = .failure(KiPadError.message("Dosya yazılamadı."))

        NSFileCoordinator().coordinate(writingItemAt: url, options: [.forReplacing],
                                       error: &coordinationError) { writeURL in
            do {
                // An atomic write needs a temporary file next to the original, which a
                // document picked from another app's folder may not allow; then write in place.
                do {
                    try data.write(to: writeURL, options: [.atomic])
                } catch {
                    try data.write(to: writeURL, options: [])
                }
                result = .success(())
            } catch {
                result = .failure(error)
            }
        }

        if let coordinationError {
            throw coordinationError
        }

        try result.get()
    }
}
