// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.

import SwiftUI

/// Floating card describing the selected item, with net highlight and focus actions.
struct SelectionCard: View {
    @EnvironmentObject private var store: BoardStore
    let selection: Selection

    private var hasNet: Bool { selection.item.net > 0 }
    private var isNetOnly: Bool { selection.item.itemType == .nothing }

    private var showsNetSection: Bool {
        store.highlightWholeNet && !isNetOnly && selection.netInfo != nil
    }

    private var rowCount: Int {
        selection.info.rows.count + (showsNetSection ? (selection.netInfo?.rows.count ?? 0) + 2 : 0)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header

            Divider()

            // short descriptions size to their content; long ones scroll in a fixed area
            // (a bare ScrollView would stretch the card to the full screen height)
            if rowCount <= 13 {
                rows
            } else {
                ScrollView { rows }
                    .frame(height: 380)
            }

            Divider()

            actions
        }
        .frame(maxWidth: 380)
        .background(Color(uiColor: .systemBackground).opacity(0.97),
                    in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color(uiColor: .separator)))
        .shadow(color: .black.opacity(0.3), radius: 12, y: 4)
    }

    private var rows: some View {
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
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(.tint)

            VStack(alignment: .leading, spacing: 2) {
                Text(selection.info.title.isEmpty ? String(localized: "Selection") : selection.info.title)
                    .font(.headline)
                    .lineLimit(2)

                Text(kindName)
                    .font(.caption)
                    .foregroundStyle(Color(uiColor: .secondaryLabel))
            }

            Spacer()

            Button {
                store.clearSelection()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(Color(uiColor: .tertiaryLabel))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close")
        }
        .padding(14)
    }

    private var actions: some View {
        HStack(spacing: 8) {
            if hasNet && !isNetOnly {
                Button {
                    store.highlightWholeNet.toggle()
                } label: {
                    Label(store.highlightWholeNet ? "Highlight item" : "Highlight net",
                          systemImage: store.highlightWholeNet ? "scope" : "point.3.connected.trianglepath.dotted")
                        .font(.callout)
                }
                .buttonStyle(.bordered)
            }

            Button {
                store.focusOnSelection()
            } label: {
                Label("Focus", systemImage: "viewfinder")
                    .font(.callout)
            }
            .buttonStyle(.bordered)

            if let sheet = store.selectedSheetTarget {
                Button {
                    store.openSheet(sheet)
                } label: {
                    Label("Open sheet", systemImage: "arrow.down.right.square")
                        .font(.callout)
                }
                .buttonStyle(.borderedProminent)
            }

            let others = store.selectedNetOtherSheets

            if !others.isEmpty {
                Menu {
                    ForEach(others) { sheet in
                        Button(sheet.page.isEmpty ? sheet.title : "\(sheet.title) (\(sheet.page))") {
                            store.showNetOnSheet(sheet.id)
                        }
                    }
                } label: {
                    Label("Other sheets", systemImage: "rectangle.stack")
                        .font(.callout)
                }
                .buttonStyle(.bordered)
            }

            if selection.candidates.count > 1 {
                Button {
                    store.nextCandidate()
                } label: {
                    Label("\(selection.candidateIndex + 1)/\(selection.candidates.count)",
                          systemImage: "square.3.layers.3d.down.right")
                        .font(.callout)
                }
                .buttonStyle(.bordered)
                .accessibilityLabel("Item underneath")
            }

            Spacer(minLength: 0)
        }
        .padding(12)
    }

    private var kindName: String {
        switch selection.item.itemType {
        case .pad: return String(localized: "Pad")
        case .via: return String(localized: "Via")
        case .track: return String(localized: "Track")
        case .arc: return String(localized: "Arc track")
        case .footprint: return String(localized: "Footprint")
        case .zone: return String(localized: "Copper zone")
        case .shape: return String(localized: "Graphic")
        case .text: return String(localized: "Text")
        case .nothing: return String(localized: "Net")
        case .schSymbol: return String(localized: "Symbol")
        case .schPin: return String(localized: "Pin")
        case .schWire: return String(localized: "Wire")
        case .schJunction: return String(localized: "Junction")
        case .schLabel: return String(localized: "Label")
        case .schSheet: return String(localized: "Sheet")
        case .schNote: return String(localized: "Note")
        case .schSheetPin: return String(localized: "Sheet pin")
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
        case .schSymbol: return "cpu"
        case .schPin: return "line.diagonal"
        case .schWire: return "point.topleft.down.curvedto.point.bottomright.up"
        case .schJunction: return "circle.fill"
        case .schLabel: return "tag"
        case .schSheet: return "square.on.square"
        case .schNote: return "text.alignleft"
        case .schSheetPin: return "arrowtriangle.right"
        }
    }
}

private struct InfoRowView: View {
    let row: InfoRow

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(row.key)
                .font(.subheadline)
                .foregroundStyle(Color(uiColor: .secondaryLabel))
                .frame(width: 118, alignment: .leading)

            Text(row.value)
                .font(.subheadline)
                .foregroundStyle(Color(uiColor: .label))
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
                    Text("snapped to pad/via centre")
                        .font(.caption)
                        .foregroundStyle(Color(uiColor: .secondaryLabel))
                }
            } else if store.measureA != nil {
                Text("Tap the second point")
            } else {
                Text("Tap the first point to measure (snaps to pad and via centres)")
            }

            Button {
                store.clearMeasurement()
            } label: {
                Image(systemName: "arrow.counterclockwise")
            }
            .buttonStyle(.borderless)
            .disabled(store.measureA == nil)
            .accessibilityLabel("Reset measurement")

            Button {
                store.measureMode = false
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(Color.secondary)
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Close measurement")
        }
        .font(.callout)
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color(uiColor: .systemBackground).opacity(0.95), in: Capsule())
        .overlay(Capsule().stroke(Color.yellow.opacity(0.6)))
        .padding(.top, 10)
    }
}
