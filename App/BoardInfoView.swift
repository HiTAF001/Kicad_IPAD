// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import SwiftUI

struct BoardInfoView: View {
    @Environment(\.dismiss) private var dismiss
    let board: LoadedBoard

    var body: some View {
        NavigationStack {
            Form {
                Section("Dosya") {
                    row("Ad", board.name)

                    if !board.title.isEmpty {
                        row("Başlık", board.title)
                    }

                    if !board.revision.isEmpty {
                        row("Revizyon", board.revision)
                    }

                    if !board.company.isEmpty {
                        row("Şirket", board.company)
                    }

                    if !board.date.isEmpty {
                        row("Tarih", board.date)
                    }

                    row("KiCad", kicadVersionText)
                    row("Dosya biçimi", "\(board.stats.file_version)")
                    row("Boyut", ByteCountFormatter.string(fromByteCount: Int64(board.fileSize), countStyle: .file))
                }

                Section("Kart") {
                    if board.stats.width_mm > 0 {
                        row("Ölçüler", String(format: "%.2f × %.2f mm", board.stats.width_mm, board.stats.height_mm))
                    } else {
                        row("Ölçüler", "Kart kenarı (Edge.Cuts) yok")
                    }

                    row("Kalınlık", String(format: "%.2f mm", board.stats.thickness_mm))
                    row("Bakır katman", "\(board.stats.copper_layers)")
                }

                Section("İçerik") {
                    row("Footprint", "\(board.stats.footprints)")
                    row("Pad", "\(board.stats.pads)")
                    row("Pist", board.stats.arcs > 0
                        ? "\(board.stats.tracks) düz + \(board.stats.arcs) yay"
                        : "\(board.stats.tracks)")
                    row("Via", "\(board.stats.vias)")
                    row("Delik (toplam)", "\(board.stats.holes)")
                    row("Bakır bölge", board.stats.keepouts > 0
                        ? "\(board.stats.zones) (+\(board.stats.keepouts) yasak bölge)"
                        : "\(board.stats.zones)")
                    row("Net", "\(board.stats.nets)")
                    row("Kart çizimi", "\(board.stats.graphics)")
                    row("Kart yazısı", "\(board.stats.texts)")
                }

                Section("Performans") {
                    row("Okuma", String(format: "%.0f ms", board.stats.parse_ms))
                    row("Geometri", String(format: "%.0f ms", board.stats.mesh_ms))
                    row("Üçgen", NumberFormatter.localizedString(from: NSNumber(value: board.stats.triangle_count),
                                                                 number: .decimal))
                }

                if !board.warnings.isEmpty {
                    Section("Uyarılar") {
                        ForEach(Array(board.warnings.enumerated()), id: \.offset) { item in
                            Text(item.element)
                                .font(.footnote)
                        }
                    }
                }
            }
            .navigationTitle("Kart bilgisi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Kapat") { dismiss() }
                }
            }
        }
    }

    private var kicadVersionText: String {
        if board.generatorVersion.isEmpty {
            return board.generator.isEmpty ? "Bilinmiyor" : board.generator
        }

        return "\(board.generatorVersion) (\(board.generator))"
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }
}
