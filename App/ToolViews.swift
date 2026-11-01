// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.
//
// Track drawing bar, design check panel and fabrication files sheet.

import SwiftUI
import UniformTypeIdentifiers

/// A zip archive written with the file exporter.
struct ZipFile: FileDocument {
    static var readableContentTypes: [UTType] { [.zip] }
    static var writableContentTypes: [UTType] { [.zip] }

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

private func swiftColor(_ c: SIMD4<Float>) -> Color {
    Color(red: Double(c.x), green: Double(c.y), blue: Double(c.z))
}

// MARK: - Drawing bar

private struct ViaOption: Hashable {
    let size: Double
    let drill: Double
}

/// Second bar under the edit bar while tracks are drawn: layer, width, via, finger drawing.
struct RouteToolBar: View {
    @EnvironmentObject private var store: BoardStore

    private let widths: [Double] = [0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.8, 1.0, 1.5, 2.0]
    private let vias: [ViaOption] = [ViaOption(size: 0.45, drill: 0.2), ViaOption(size: 0.6, drill: 0.3),
                                     ViaOption(size: 0.8, drill: 0.4), ViaOption(size: 1.0, drill: 0.5),
                                     ViaOption(size: 1.2, drill: 0.6)]

    private var currentSlot: LayerSlot? {
        store.copperSlots.first { $0.id == store.routeSettings.slot }
    }

    var body: some View {
        HStack(spacing: 14) {
            Menu {
                Picker("Layer", selection: $store.routeSettings.slot) {
                    ForEach(store.copperSlots) { slot in
                        Text(slot.title).tag(slot.id)
                    }
                }
            } label: {
                HStack(spacing: 6) {
                    Circle()
                        .fill(currentSlot.map { swiftColor($0.color) } ?? Color.gray)
                        .frame(width: 12, height: 12)
                    Text(currentSlot?.title ?? "—")
                        .font(.subheadline.weight(.semibold))
                }
                .frame(minHeight: 34)
            }
            .accessibilityLabel("Copper layer")

            Menu {
                Picker("Track width", selection: $store.routeSettings.width) {
                    ForEach(widths, id: \.self) { w in
                        Text(String(format: "%g mm", w)).tag(w)
                    }
                }
            } label: {
                Label(String(format: "%g mm", store.routeSettings.width), systemImage: "line.diagonal")
                    .font(.subheadline.monospacedDigit())
                    .frame(minHeight: 34)
            }
            .accessibilityLabel("Track width")

            Menu {
                ForEach(vias, id: \.self) { v in
                    Button(String(format: "%g / %g mm", v.size, v.drill)) {
                        store.routeSettings.viaSize = v.size
                        store.routeSettings.viaDrill = v.drill
                    }
                }
            } label: {
                Label(String(format: "%g/%g", store.routeSettings.viaSize, store.routeSettings.viaDrill),
                      systemImage: "circle.circle")
                    .font(.subheadline.monospacedDigit())
                    .frame(minHeight: 34)
            }
            .accessibilityLabel("Via diameter and hole")

            Toggle(isOn: $store.fingerDrawing) {
                Image(systemName: "hand.point.up.left")
            }
            .toggleStyle(.button)
            .accessibilityLabel("Draw with a finger too")

            if let hint = store.routeHint {
                Text(hint)
                    .font(.footnote)
                    .foregroundStyle(store.routeHintIsProblem ? Color.red : Color.secondary)
                    .lineLimit(1)
                    .frame(maxWidth: 320, alignment: .leading)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 2)
        .background(Color(uiColor: .systemBackground).opacity(0.94), in: Capsule())
        .overlay(Capsule().stroke(Color.accentColor.opacity(0.35)))
        .shadow(color: .black.opacity(0.2), radius: 6, y: 2)
    }
}

// MARK: - Design check

private struct DRCGroup: Identifiable {
    let id: Int
    let title: String
    let isError: Bool
    let entries: [DRCEntry]
}

/// Lets the board behind a half-height sheet stay usable (iOS 16.4+).
private struct BoardStaysInteractive: ViewModifier {
    func body(content: Content) -> some View {
        if #available(iOS 16.4, *) {
            content.presentationBackgroundInteraction(.enabled(upThrough: .medium))
        } else {
            content
        }
    }
}

struct DRCView: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss

    private var groups: [DRCGroup] {
        guard let report = store.drcReport else { return [] }

        var order: [Int] = []
        var byCode: [Int: [DRCEntry]] = [:]

        for e in report.entries {
            if byCode[e.code] == nil {
                order.append(e.code)
            }

            byCode[e.code, default: []].append(e)
        }

        return order.compactMap { code in
            guard let list = byCode[code], let first = list.first else { return nil }
            return DRCGroup(id: code, title: first.title, isError: first.isError, entries: list)
        }
    }

    private var rulesText: String {
        guard let report = store.drcReport else { return "" }

        switch report.rulesSource {
        case 2:
            return store.projectRulesName.map { String(localized: "Rules: \($0)") } ?? String(localized: "Rules: project file")
        case 1:
            return String(localized: "Rules: from the board file (KiCad 5)")
        default:
            return String(localized: "Rules: KiCad defaults")
        }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if store.drcRunning {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text("Checking…")
                        }
                    } else if let report = store.drcReport {
                        HStack(spacing: 16) {
                            Label {
                                Text(verbatim: "\(report.errors)")
                            } icon: {
                                Image(systemName: "xmark.octagon.fill")
                            }
                            .foregroundStyle(report.errors > 0 ? Color.red : Color.secondary)
                            .accessibilityLabel("Errors: \(String(report.errors))")

                            Label {
                                Text(verbatim: "\(report.warnings)")
                            } icon: {
                                Image(systemName: "exclamationmark.triangle.fill")
                            }
                            .foregroundStyle(report.warnings > 0 ? Color.orange : Color.secondary)
                            .accessibilityLabel("Warnings: \(String(report.warnings))")
                            Spacer()
                            Text(String(format: "%.0f ms", report.milliseconds))
                                .font(.caption.monospacedDigit())
                                .foregroundStyle(.secondary)
                        }
                        .font(.headline)

                        if store.drcStale {
                            Label("The board changed since this check", systemImage: "clock.arrow.circlepath")
                                .foregroundStyle(.orange)
                        }

                        Text(rulesText)
                            .font(.footnote)
                            .foregroundStyle(.secondary)

                        if !report.note.isEmpty {
                            Text(report.note)
                                .font(.footnote)
                                .foregroundStyle(.orange)
                        }
                    } else {
                        Text("Checks clearances, shorts, holes, the board edge, courtyards and missing connections.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }

                    Button {
                        store.showRulesImporter = true
                    } label: {
                        Label(store.projectRulesName == nil ? "Use rules from a KiCad project file…" as LocalizedStringKey
                                                            : "Choose another project file…",
                              systemImage: "doc.badge.gearshape")
                    }
                }

                if let report = store.drcReport, report.entries.isEmpty, !store.drcRunning {
                    Section {
                        Label("No problems found", systemImage: "checkmark.seal.fill")
                            .foregroundStyle(.green)
                            .font(.headline)
                    }
                }

                ForEach(groups) { group in
                    Section {
                        ForEach(group.entries.prefix(300)) { entry in
                            Button {
                                store.focusDRC(entry)
                            } label: {
                                HStack(alignment: .firstTextBaseline, spacing: 10) {
                                    Image(systemName: entry.isError ? "xmark.octagon.fill" : "exclamationmark.triangle.fill")
                                        .foregroundStyle(entry.isError ? Color.red : Color.orange)
                                    Text(entry.message)
                                        .font(.callout)
                                        .foregroundStyle(Color.primary)
                                        .multilineTextAlignment(.leading)
                                    Spacer(minLength: 0)

                                    if store.drcFocusID == entry.id {
                                        Image(systemName: "scope")
                                            .foregroundStyle(Color.accentColor)
                                    }
                                }
                            }
                        }

                        if group.entries.count > 300 {
                            Text("… and \(String(group.entries.count - 300)) more")
                                .foregroundStyle(.secondary)
                        }
                    } header: {
                        Text(verbatim: "\(group.title) (\(group.entries.count))")
                    }
                }
            }
            .navigationTitle("Design check")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button {
                        store.runDRC()
                    } label: {
                        Label("Run again", systemImage: "arrow.clockwise")
                    }
                    .disabled(store.drcRunning || store.board == nil)
                }

                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .fileImporter(isPresented: $store.showRulesImporter, allowedContentTypes: [.data],
                          allowsMultipleSelection: false) { result in
                store.importRules(result)
            }
            .onAppear {
                if store.drcReport == nil || store.drcStale {
                    store.runDRC()
                }
            }
        }
        .presentationDetents([.medium, .large])
        .modifier(BoardStaysInteractive())
    }
}

// MARK: - Fabrication files

struct FabView: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss

    private func icon(_ kind: String) -> String {
        switch kind {
        case "gerber": return "square.3.layers.3d"
        case "drill": return "circle.grid.cross"
        case "bom": return "list.bullet.rectangle"
        case "positions": return "scope"
        default: return "doc"
        }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text("Gerber (RS-274X X2) for every copper, solder mask, paste and silkscreen layer and the board outline, Excellon drill files, a bill of materials and a placement file. Standard files every board house accepts.")
                        .font(.callout)
                        .foregroundStyle(.secondary)

                    Toggle("Coordinates from the drill/place origin", isOn: $store.fabUseAuxOrigin)
                }

                Section {
                    if store.drcRunning {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text("Checking…")
                        }
                    } else if let report = store.drcReport, !store.drcStale {
                        Label(report.errors == 0 ? "Design check: no errors" as LocalizedStringKey
                                                 : "Design check: \(String(report.errors)) errors",
                              systemImage: report.errors == 0 ? "checkmark.seal.fill" : "xmark.octagon.fill")
                            .foregroundStyle(report.errors == 0 ? Color.green : Color.red)
                    } else {
                        Button {
                            store.runDRC()
                        } label: {
                            Label("Run the design check first", systemImage: "checkmark.shield")
                        }
                    }
                }

                if store.fabRunning {
                    Section {
                        HStack(spacing: 10) {
                            ProgressView()
                            Text("Making the files…")
                        }
                    }
                }

                if let package = store.fabPackage, !store.fabRunning {
                    if !package.warnings.isEmpty {
                        Section {
                            ForEach(package.warnings, id: \.self) { w in
                                Label(w, systemImage: "exclamationmark.triangle.fill")
                                    .foregroundStyle(.orange)
                            }
                        }
                    }

                    Section {
                        ForEach(package.files) { f in
                            HStack(spacing: 10) {
                                Image(systemName: icon(f.kind))
                                    .foregroundStyle(Color.accentColor)
                                    .frame(width: 24)
                                Text(f.name)
                                    .font(.callout.monospaced())
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                Spacer()
                                Text(ByteCountFormatter.string(fromByteCount: Int64(f.size), countStyle: .file))
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    } header: {
                        Text("Files")
                    }
                }
            }
            .navigationTitle("Fabrication files")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }

                ToolbarItem(placement: .confirmationAction) {
                    Button {
                        store.exportFabrication()
                    } label: {
                        Label("Save ZIP…", systemImage: "square.and.arrow.up")
                    }
                    .disabled(store.fabPackage?.zip.isEmpty ?? true || store.fabRunning)
                }
            }
            .fileExporter(isPresented: $store.showFabExporter, document: store.fabZipFile, contentType: .zip,
                          defaultFilename: store.documentName + "-fab.zip") { result in
                store.fabExportFinished(result)
            }
            .onAppear {
                if store.fabPackage == nil || store.fabStale {
                    store.makeFabrication()
                }
            }
            .onChange(of: store.fabUseAuxOrigin) { _ in
                store.makeFabrication()
            }
        }
        .presentationDetents([.medium, .large])
    }
}
