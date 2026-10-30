// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import SwiftUI

struct LayerPanel: View {
    @EnvironmentObject private var store: BoardStore

    var body: some View {
        NavigationStack {
            List {
                Section("Hızlı seçim") {
                    Button {
                        store.showAllLayers()
                    } label: {
                        Label("Tüm katmanlar", systemImage: "square.3.layers.3d")
                    }

                    Button {
                        store.showCopperOnly()
                    } label: {
                        Label("Sadece bakır", systemImage: "bolt.horizontal")
                    }

                    Button {
                        store.showSide(front: true)
                    } label: {
                        Label("Sadece ön yüz", systemImage: "arrow.up.square")
                    }

                    Button {
                        store.showSide(front: false)
                    } label: {
                        Label("Sadece arka yüz", systemImage: "arrow.down.square")
                    }

                    Toggle(isOn: $store.flipped) {
                        Label("Alttan görünüm", systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right")
                    }
                }

                Section("Nesneler") {
                    ForEach(ObjectCategory.allCases) { category in
                        Toggle(isOn: Binding(get: { store.isShown(category) },
                                             set: { store.setShown(category, $0) })) {
                            Label(category.title, systemImage: category.systemImage)
                        }
                    }
                }

                let copper = slots(of: Self.copperKinds, includeEmpty: true)
                let technical = slots(of: Self.technicalKinds, includeEmpty: false)
                let other = slots(of: Self.otherKinds, includeEmpty: false)

                if !copper.isEmpty {
                    Section("Bakır katmanlar") {
                        ForEach(copper) { slot in LayerRow(slot: slot) }
                    }
                }

                if !technical.isEmpty {
                    Section("Teknik katmanlar") {
                        ForEach(technical) { slot in LayerRow(slot: slot) }
                    }
                }

                if !other.isEmpty {
                    Section("Kenar ve kullanıcı katmanları") {
                        ForEach(other) { slot in LayerRow(slot: slot) }
                    }
                }
            }
            .navigationTitle("Katmanlar")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private static let copperKinds: Set<LayerKind> = [.copper]
    private static let technicalKinds: Set<LayerKind> = [.silk, .mask, .paste, .adhesive, .fab, .courtyard]
    private static let otherKinds: Set<LayerKind> = [.edge, .margin, .user]

    /// Slots of the given kinds, front-most first.
    private func slots(of kinds: Set<LayerKind>, includeEmpty: Bool) -> [LayerSlot] {
        guard let board = store.board else { return [] }

        let matching: [LayerSlot] = board.slots.filter { slot in
            kinds.contains(slot.kind) && (includeEmpty || slot.hasContent)
        }

        return matching.sorted { a, b in a.sortFront > b.sortFront }
    }
}

private struct LayerRow: View {
    @EnvironmentObject private var store: BoardStore
    let slot: LayerSlot

    var body: some View {
        Toggle(isOn: Binding(get: { store.isVisible(slot) },
                             set: { store.setVisible(slot, $0) })) {
            HStack(spacing: 12) {
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color(.sRGB,
                                red: Double(slot.color.x),
                                green: Double(slot.color.y),
                                blue: Double(slot.color.z),
                                opacity: Double(max(slot.color.w, 0.5))))
                    .frame(width: 22, height: 22)
                    .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.4)))

                VStack(alignment: .leading, spacing: 1) {
                    Text(slot.title)
                        .foregroundStyle(slot.hasContent ? Color.primary : Color.secondary)

                    if let subtitle = slot.subtitle {
                        Text(subtitle)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}
