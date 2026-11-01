// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.
//
// Edit mode: the tool bar, the small editing sheets and the toast messages.

import SwiftUI

/// Floating bar at the top of the board while editing.
struct EditBar: View {
    @EnvironmentObject private var store: BoardStore

    private var selectedType: ItemType { store.selection?.item.itemType ?? .nothing }

    var body: some View {
        HStack(spacing: 4) {
            barButton(store.docState.undoLabel.isEmpty ? String(localized: "Undo")
                                                       : String(localized: "Undo: \(store.docState.undoLabel)"),
                      "arrow.uturn.left", enabled: store.docState.canUndo) { store.undo() }

            barButton(store.docState.redoLabel.isEmpty ? String(localized: "Redo")
                                                       : String(localized: "Redo: \(store.docState.redoLabel)"),
                      "arrow.uturn.right", enabled: store.docState.canRedo) { store.redo() }

            separator

            barButton(String(localized: "Rotate 90° left"), "rotate.left", enabled: store.editFootprint != nil) {
                store.rotateSelection(by: 90)
            }

            barButton(String(localized: "Rotate 90° right"), "rotate.right", enabled: store.editFootprint != nil) {
                store.rotateSelection(by: -90)
            }

            barButton(store.deleteTarget.map { String(localized: "Delete: \($0.title)") } ?? String(localized: "Delete"), "trash",
                      enabled: store.deleteTarget != nil, role: .destructive) {
                store.deleteSelection()
            }

            if store.editFootprint != nil {
                barButton(String(localized: "Reference and value"), "character.cursor.ibeam", enabled: true) {
                    store.activeSheet = .fields
                }
            } else if selectedType == .track || selectedType == .arc {
                barButton(String(localized: "Track width"), "line.horizontal.3.decrease", enabled: true) {
                    store.activeSheet = .trackWidth
                }
            } else if selectedType == .via {
                barButton(String(localized: "Via diameter and hole"), "circle.circle", enabled: true) {
                    store.activeSheet = .via
                }
            }

            separator

            Menu {
                Picker("Grid", selection: $store.gridStep) {
                    ForEach(gridSteps, id: \.self) { step in
                        Text(gridTitle(step)).tag(step)
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: "grid")
                    Text(store.gridStep > 0 ? String(format: "%g", store.gridStep) : "—")
                        .font(.caption.monospacedDigit())
                }
                .frame(minWidth: 44, minHeight: 36)
            }
            .accessibilityLabel("Grid: \(gridTitle(store.gridStep))")

            separator

            Menu {
                Button {
                    store.save()
                } label: {
                    Label(store.canSaveInPlace ? "Save" as LocalizedStringKey : "Save…", systemImage: "square.and.arrow.down")
                }

                Button {
                    store.saveAs()
                } label: {
                    Label("Save as…", systemImage: "square.and.arrow.down.on.square")
                }
            } label: {
                ZStack(alignment: .topTrailing) {
                    Image(systemName: "square.and.arrow.down")
                        .frame(minWidth: 40, minHeight: 36)

                    if store.docState.modified {
                        Circle()
                            .fill(Color.orange)
                            .frame(width: 8, height: 8)
                            .offset(x: -4, y: 4)
                    }
                }
            } primaryAction: {
                store.save()
            }
            .disabled(!store.canEdit)
            .accessibilityLabel(store.docState.modified ? "Save (unsaved changes)" as LocalizedStringKey : "Save")

            if store.isRebuilding {
                ProgressView()
                    .controlSize(.small)
                    .frame(width: 28)
                    .accessibilityLabel("Updating the board")
            }

            Button {
                store.editMode = false
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(Color(uiColor: .tertiaryLabel))
                    .frame(minWidth: 36, minHeight: 36)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Finish editing")
        }
        .font(.title3)
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .background(Color(uiColor: .systemBackground).opacity(0.96), in: Capsule())
        .overlay(Capsule().stroke(Color.accentColor.opacity(0.5)))
        .shadow(color: .black.opacity(0.25), radius: 8, y: 3)
        .padding(.top, 10)
    }

    private var separator: some View {
        Rectangle()
            .fill(Color(uiColor: .separator))
            .frame(width: 1, height: 24)
            .padding(.horizontal, 4)
    }

    private func barButton(_ title: String, _ icon: String, enabled: Bool, role: ButtonRole? = nil,
                           action: @escaping () -> Void) -> some View {
        Button(role: role, action: action) {
            Image(systemName: icon)
                .frame(minWidth: 40, minHeight: 36)
        }
        .disabled(!enabled || !store.canEdit)
        .help(title)
        .accessibilityLabel(title)
    }
}

// MARK: - Sheets

/// Accepts "0,25" as well as "0.25".
func parseMillimetres(_ text: String) -> Double? {
    let t = text.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".")
    guard let v = Double(t), v.isFinite, v > 0 else { return nil }
    return v
}

func formatMillimetres(_ v: Double) -> String {
    String(format: "%g", (v * 1_000_000).rounded() / 1_000_000)
}

struct FieldEditSheet: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss
    @State private var reference = ""
    @State private var value = ""
    @State private var loaded = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Reference (e.g. R12)", text: $reference)
                        .textInputAutocapitalization(.characters)
                        .autocorrectionDisabled()
                    TextField("Value (e.g. 10k)", text: $value)
                        .autocorrectionDisabled()
                } footer: {
                    Text("Text position and size stay the same; only the text changes.")
                }
            }
            .navigationTitle("Reference and value")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") {
                        store.setFields(reference: reference, value: value)
                        dismiss()
                    }
                    .disabled(reference.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            .onAppear {
                guard !loaded, let board = store.board, let fp = store.editFootprint,
                      fp < board.footprints.count else { return }
                loaded = true
                reference = board.footprints[fp].reference
                value = board.footprints[fp].value
            }
        }
    }
}

struct TrackWidthSheet: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var loaded = false

    private let presets: [Double] = [0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.8, 1.0, 1.5, 2.0]

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack {
                        TextField("Width", text: $text)
                            .keyboardType(.decimalPad)
                        Text(verbatim: "mm").foregroundStyle(.secondary)
                    }
                } footer: {
                    Text("Only the selected track segment changes. Copper zone fills are not recalculated; refill zones in KiCad (B).")
                }

                Section("Presets") {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 64))], spacing: 8) {
                        ForEach(presets, id: \.self) { p in
                            Button(formatMillimetres(p)) { text = formatMillimetres(p) }
                                .buttonStyle(.bordered)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle("Track width")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") {
                        if let w = parseMillimetres(text) {
                            store.setTrackWidth(w)
                        }
                        dismiss()
                    }
                    .disabled(parseMillimetres(text) == nil)
                }
            }
            .onAppear {
                guard !loaded, let board = store.board, let item = store.selection?.item else { return }
                loaded = true
                text = formatMillimetres(board.trackWidth(item))
            }
        }
    }
}

struct ViaSheet: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss
    @State private var sizeText = ""
    @State private var drillText = ""
    @State private var loaded = false

    private let presets: [(Double, Double)] = [(0.45, 0.2), (0.6, 0.3), (0.8, 0.4), (1.0, 0.5), (1.2, 0.6)]

    private var valid: Bool {
        guard let s = parseMillimetres(sizeText), let d = parseMillimetres(drillText) else { return false }
        return d < s
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack {
                        Text("Diameter")
                        TextField("Diameter", text: $sizeText)
                            .keyboardType(.decimalPad)
                            .multilineTextAlignment(.trailing)
                        Text(verbatim: "mm").foregroundStyle(.secondary)
                    }
                    HStack {
                        Text("Hole")
                        TextField("Hole", text: $drillText)
                            .keyboardType(.decimalPad)
                            .multilineTextAlignment(.trailing)
                        Text(verbatim: "mm").foregroundStyle(.secondary)
                    }
                } footer: {
                    Text(valid || sizeText.isEmpty ? "The hole must be smaller than the via diameter." as LocalizedStringKey
                                                   : "The hole must be smaller than the via diameter — check the values.")
                }

                Section("Presets") {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 96))], spacing: 8) {
                        ForEach(presets.indices, id: \.self) { i in
                            let p = presets[i]
                            Button("\(formatMillimetres(p.0)) / \(formatMillimetres(p.1))" as String) {
                                sizeText = formatMillimetres(p.0)
                                drillText = formatMillimetres(p.1)
                            }
                            .buttonStyle(.bordered)
                        }
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle("Via")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") {
                        if let s = parseMillimetres(sizeText), let d = parseMillimetres(drillText) {
                            store.setVia(size: s, drill: d)
                        }
                        dismiss()
                    }
                    .disabled(!valid)
                }
            }
            .onAppear {
                guard !loaded, let board = store.board, let item = store.selection?.item,
                      item.itemType == .via, let g = board.viaGeometry(Int(item.index)) else { return }
                loaded = true
                sizeText = formatMillimetres(g.size)
                drillText = formatMillimetres(g.drill)
            }
        }
    }
}

// MARK: - Toast

struct ToastView: View {
    @EnvironmentObject private var store: BoardStore
    let toast: Toast

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: toast.isError ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                .foregroundStyle(toast.isError ? Color.orange : Color.green)

            Text(toast.text)
                .font(.callout)
                .foregroundStyle(Color(uiColor: .label))
                .lineLimit(3)

            if toast.offersUndo && store.docState.canUndo {
                Button("Undo") {
                    store.dismissToast()
                    store.undo()
                }
                .font(.callout.weight(.semibold))
                .disabled(!store.canEdit)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .frame(maxWidth: 560)
        .background(Color(uiColor: .systemBackground).opacity(0.97), in: Capsule())
        .overlay(Capsule().stroke(toast.isError ? Color.orange.opacity(0.7) : Color(uiColor: .separator)))
        .shadow(color: .black.opacity(0.25), radius: 8, y: 3)
        .onTapGesture { store.dismissToast() }
    }
}
