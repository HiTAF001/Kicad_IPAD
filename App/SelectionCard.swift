// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import SwiftUI

/// Floating card describing the selected item, with net highlight and focus actions.
struct SelectionCard: View {
    @EnvironmentObject private var store: BoardStore
    let selection: Selection

    private var hasNet: Bool { selection.item.net > 0 }
    private var isNetOnly: Bool { selection.item.itemType == .nothing }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(selection.info.rows) { row in
                        InfoRowView(row: row)
                    }

                    if store.highlightWholeNet, !isNetOnly, let netInfo = selection.netInfo {
                        Divider().padding(.vertical, 4)

                        Text("Net: \(netInfo.title)")
                            .font(.subheadline.weight(.semibold))

                        ForEach(netInfo.rows) { row in
                            InfoRowView(row: row)
                        }
                    }
                }
                .padding(14)
            }

            Divider()

            actions
        }
        .frame(maxWidth: 380)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color.secondary.opacity(0.25)))
        .shadow(color: .black.opacity(0.25), radius: 12, y: 4)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(.tint)

            VStack(alignment: .leading, spacing: 2) {
                Text(selection.info.title.isEmpty ? "Seçim" : selection.info.title)
                    .font(.headline)
                    .lineLimit(2)

                Text(kindName)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button {
                store.clearSelection()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(Color.secondary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Kapat")
        }
        .padding(14)
    }

    private var actions: some View {
        HStack(spacing: 8) {
            if hasNet && !isNetOnly {
                Button {
                    store.highlightWholeNet.toggle()
                } label: {
                    Label(store.highlightWholeNet ? "Öğeyi vurgula" : "Neti vurgula",
                          systemImage: store.highlightWholeNet ? "scope" : "point.3.connected.trianglepath.dotted")
                        .font(.callout)
                }
                .buttonStyle(.bordered)
            }

            Button {
                store.focusOnSelection()
            } label: {
                Label("Odaklan", systemImage: "viewfinder")
                    .font(.callout)
            }
            .buttonStyle(.bordered)

            if selection.candidates.count > 1 {
                Button {
                    store.nextCandidate()
                } label: {
                    Label("\(selection.candidateIndex + 1)/\(selection.candidates.count)",
                          systemImage: "square.3.layers.3d.down.right")
                        .font(.callout)
                }
                .buttonStyle(.bordered)
                .accessibilityLabel("Altındaki öğe")
            }

            Spacer(minLength: 0)
        }
        .padding(12)
    }

    private var kindName: String {
        switch selection.item.itemType {
        case .pad: return "Pad"
        case .via: return "Via"
        case .track: return "Pist"
        case .arc: return "Yay pist"
        case .footprint: return "Footprint"
        case .zone: return "Bakır bölge"
        case .shape: return "Çizim"
        case .text: return "Yazı"
        case .nothing: return "Net"
        }
    }

    private var icon: String {
        switch selection.item.itemType {
        case .pad: return "square.fill"
        case .via: return "circle.circle"
        case .track, .arc: return "point.topleft.down.curvedto.point.bottomright.up"
        case .footprint: return "cpu"
        case .zone: return "square.fill.on.square.fill"
        case .shape: return "pencil.and.outline"
        case .text: return "textformat"
        case .nothing: return "point.3.connected.trianglepath.dotted"
        }
    }
}

private struct InfoRowView: View {
    let row: InfoRow

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(row.key)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .frame(width: 118, alignment: .leading)

            Text(row.value)
                .font(.subheadline)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// Bar shown while measuring.
struct MeasureBar: View {
    @EnvironmentObject private var store: BoardStore

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "ruler")
                .foregroundStyle(.yellow)

            if let summary = store.measurementSummary {
                Text(summary)
                    .monospacedDigit()
                    .fontWeight(.semibold)

                if store.measureSnapA || store.measureSnapB {
                    Text("pad/via merkezine tuttu")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else if store.measureA != nil {
                Text("İkinci noktaya dokun")
            } else {
                Text("Ölçmek için ilk noktaya dokun (pad ve via merkezlerine tutunur)")
            }

            Button {
                store.clearMeasurement()
            } label: {
                Image(systemName: "arrow.counterclockwise")
            }
            .buttonStyle(.borderless)
            .disabled(store.measureA == nil)
            .accessibilityLabel("Ölçümü sıfırla")

            Button {
                store.measureMode = false
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(Color.secondary)
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Ölçümü kapat")
        }
        .font(.callout)
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.regularMaterial, in: Capsule())
        .overlay(Capsule().stroke(Color.yellow.opacity(0.6)))
        .padding(.top, 10)
    }
}
