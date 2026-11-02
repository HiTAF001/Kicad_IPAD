// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.

import SwiftUI

struct BoardInfoView: View {
    @Environment(\.dismiss) private var dismiss
    let board: LoadedBoard

    var body: some View {
        NavigationStack {
            Form {
                Section("File") {
                    row("Name", board.name)

                    if !board.title.isEmpty {
                        row("Title", board.title)
                    }

                    if !board.revision.isEmpty {
                        row("Revision", board.revision)
                    }

                    if !board.company.isEmpty {
                        row("Company", board.company)
                    }

                    if !board.date.isEmpty {
                        row("Date", board.date)
                    }

                    if board.isFabrication {
                        row("Type", String(localized: "Gerber and drill files (view only)"))
                    } else {
                        if board.isSchematic {
                            row("Type", String(localized: "Schematic (view only)"))
                        }

                        row("KiCad", kicadVersionText)
                        row("File format", "\(board.stats.file_version)")
                    }

                    row("Size", ByteCountFormatter.string(fromByteCount: Int64(board.fileSize), countStyle: .file))
                }

                if board.isSchematic {
                    schematicSections
                } else {
                    boardSections
                }

                Section("Performance") {
                    row("Parsing", String(format: "%.0f ms", board.stats.parse_ms))
                    row("Geometry", String(format: "%.0f ms", board.stats.mesh_ms))
                    row("Connectivity", String(format: "%.0f ms", board.stats.conn_ms))
                    row("Triangles", NumberFormatter.localizedString(from: NSNumber(value: board.stats.triangle_count),
                                                                 number: .decimal))
                }

                if !board.warnings.isEmpty {
                    Section("Warnings") {
                        ForEach(Array(board.warnings.enumerated()), id: \.offset) { item in
                            Text(item.element)
                                .font(.footnote)
                        }
                    }
                }
            }
            .navigationTitle(board.isSchematic ? "Schematic info" as LocalizedStringKey : "Board info")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
    }

    private var kicadVersionText: String {
        if board.generatorVersion.isEmpty {
            return board.generator.isEmpty ? String(localized: "Unknown") : board.generator
        }

        return "\(board.generatorVersion) (\(board.generator))"
    }

    @ViewBuilder
    private var boardSections: some View {
        Section("Board") {
            if board.stats.width_mm > 0 {
                row("Dimensions", String(format: "%.2f × %.2f mm", board.stats.width_mm, board.stats.height_mm))
            } else {
                row("Dimensions", String(localized: "No board outline (Edge.Cuts)"))
            }

            if !board.isFabrication {
                row("Thickness", String(format: "%.2f mm", board.stats.thickness_mm))
            }

            row("Copper layers", "\(board.stats.copper_layers)")
        }

        if board.isFabrication {
            Section("Contents") {
                row("Drill holes", "\(board.stats.holes)")
                row("Drawn objects", "\(board.stats.graphics)")
            }
        } else {
            kicadContents
        }
    }

    @ViewBuilder
    private var schematicSections: some View {
        Section("Schematic") {
            row("Sheets", "\(board.sheets.count)")

            if let sheet = board.currentSheet {
                row("This sheet", sheet.page.isEmpty ? sheet.title : "\(sheet.title) (\(sheet.page))")

                if !sheet.file.isEmpty {
                    row("File", sheet.file)
                }
            }

            row("Paper", String(format: "%.0f × %.0f mm", board.stats.width_mm, board.stats.height_mm))
            row("Nets", "\(board.stats.nets)")
        }

        Section("On this sheet") {
            row("Symbols", "\(board.stats.footprints)")
            row("Pins", "\(board.stats.pads)")
            row("Wires", "\(board.stats.tracks)")
            row("Labels", "\(board.stats.texts)")
        }

        if !board.missingSheetFiles.isEmpty {
            Section("Missing sub-sheet files") {
                ForEach(board.missingSheetFiles, id: \.self) { file in
                    Text(verbatim: file)
                        .font(.footnote.monospaced())
                }

                Text("Open the project folder to see these sheets.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var kicadContents: some View {
        Section("Contents") {
            row("Footprints", "\(board.stats.footprints)")
            row("Pads", "\(board.stats.pads)")
            row("Tracks", board.stats.arcs > 0
                ? String(localized: "\(Int(board.stats.tracks)) straight + \(Int(board.stats.arcs)) arc")
                : "\(board.stats.tracks)")
            row("Vias", "\(board.stats.vias)")
            row("Holes (total)", "\(board.stats.holes)")
            row("Copper zones", board.stats.keepouts > 0
                ? String(localized: "\(Int(board.stats.zones)) (+\(Int(board.stats.keepouts)) keepout)")
                : "\(board.stats.zones)")
            row("Nets", "\(board.stats.nets)")
            row("Unconnected", board.stats.unconnected == 0
                ? String(localized: "None — all connections complete")
                : String(localized: "\(Int(board.stats.unconnected)) connections missing (ratsnest)"))
            row("Board graphics", "\(board.stats.graphics)")
            row("Board texts", "\(board.stats.texts)")
        }
    }

    private func row(_ title: LocalizedStringKey, _ value: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }
}
