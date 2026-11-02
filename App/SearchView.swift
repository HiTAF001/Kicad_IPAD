// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.

import SwiftUI

/// Find a footprint (reference / value) or a net and jump to it.
struct SearchView: View {
    @EnvironmentObject private var store: BoardStore
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var scope = Scope.footprints

    enum Scope: String, CaseIterable, Identifiable {
        case footprints, nets

        var id: String { rawValue }
    }

    private func title(_ scope: Scope) -> LocalizedStringKey {
        if scope == .nets {
            return "Net"
        }

        return store.isSchematicView ? "Symbol" : "Footprint"
    }

    private static let limit = 300

    var body: some View {
        NavigationStack {
            List {
                Picker("Search", selection: $scope) {
                    ForEach(Scope.allCases) { s in
                        Text(title(s)).tag(s)
                    }
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)

                if let board = store.board {
                    switch scope {
                    case .footprints where board.isSchematic:
                        symbolResults(board)
                    case .footprints:
                        footprintResults(board)
                    case .nets:
                        netResults(board)
                    }
                }
            }
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always),
                        prompt: scope == .footprints ? "R12, U3, 10k…" : "GND, VCC, SDA…")
            .autocorrectionDisabled()
            .textInputAutocapitalization(.never)
            .navigationTitle("Search")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
    }

    @ViewBuilder
    private func footprintResults(_ board: LoadedBoard) -> some View {
        let matches = filteredFootprints(board)

        if matches.isEmpty {
            Text("No matching footprint")
                .foregroundStyle(.secondary)
        }

        ForEach(matches) { fp in
            Button {
                store.selectFootprint(fp.id)
                dismiss()
            } label: {
                HStack {
                    Text(fp.reference.isEmpty ? String(localized: "(unnamed)") : fp.reference)
                        .font(.body.monospaced())
                        .frame(minWidth: 70, alignment: .leading)
                    Text(fp.value)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Spacer()
                    if fp.back {
                        Text("back")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
            }
            .foregroundStyle(Color.primary)
        }
    }

    /// Symbols of every sheet of a schematic.
    @ViewBuilder
    private func symbolResults(_ board: LoadedBoard) -> some View {
        let matches = filteredSymbols(board)

        if matches.isEmpty {
            Text("No matching symbol")
                .foregroundStyle(.secondary)
        }

        ForEach(matches) { symbol in
            Button {
                store.selectSchematicSymbol(symbol)
                dismiss()
            } label: {
                HStack {
                    Text(symbol.reference.isEmpty ? String(localized: "(unnamed)") : symbol.reference)
                        .font(.body.monospaced())
                        .frame(minWidth: 70, alignment: .leading)
                    Text(symbol.value)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Spacer()

                    if board.sheets.count > 1, symbol.sheet < board.sheets.count {
                        Text(board.sheets[symbol.sheet].title)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
            }
            .foregroundStyle(Color.primary)
        }
    }

    private func filteredSymbols(_ board: LoadedBoard) -> [SchematicSymbolEntry] {
        let q = query.trimmingCharacters(in: .whitespaces)
        let all = board.schematicSymbols
        let order: (SchematicSymbolEntry, SchematicSymbolEntry) -> Bool = {
            $0.reference.localizedStandardCompare($1.reference) == .orderedAscending
        }

        guard !q.isEmpty else {
            return Array(all.sorted(by: order).prefix(Self.limit))
        }

        let exact = all.filter { $0.reference.caseInsensitiveCompare(q) == .orderedSame }
        let prefix = all.filter {
            $0.reference.lowercased().hasPrefix(q.lowercased()) && $0.reference.caseInsensitiveCompare(q) != .orderedSame
        }.sorted(by: order)
        let rest = all.filter {
            !$0.reference.lowercased().hasPrefix(q.lowercased())
                && ($0.reference.localizedCaseInsensitiveContains(q) || $0.value.localizedCaseInsensitiveContains(q))
        }.sorted(by: order)

        return Array((exact + prefix + rest).prefix(Self.limit))
    }

    @ViewBuilder
    private func netResults(_ board: LoadedBoard) -> some View {
        let matches = filteredNets(board)

        if matches.isEmpty {
            Text("No matching net")
                .foregroundStyle(.secondary)
        }

        ForEach(matches) { net in
            Button {
                store.goToNet(net.id)
                dismiss()
            } label: {
                Text(net.name)
                    .font(.body.monospaced())
                    .lineLimit(1)
            }
            .foregroundStyle(Color.primary)
        }
    }

    private func filteredFootprints(_ board: LoadedBoard) -> [FootprintEntry] {
        let q = query.trimmingCharacters(in: .whitespaces)
        let all = board.footprints

        guard !q.isEmpty else {
            return Array(all.sorted(by: Self.naturalOrder).prefix(Self.limit))
        }

        // exact reference first, then reference prefix, then anything containing the text
        let exact = all.filter { $0.reference.caseInsensitiveCompare(q) == .orderedSame }
        let prefix = all.filter {
            $0.reference.lowercased().hasPrefix(q.lowercased()) && $0.reference.caseInsensitiveCompare(q) != .orderedSame
        }.sorted(by: Self.naturalOrder)
        let rest = all.filter {
            !$0.reference.lowercased().hasPrefix(q.lowercased())
                && ($0.reference.localizedCaseInsensitiveContains(q) || $0.value.localizedCaseInsensitiveContains(q))
        }.sorted(by: Self.naturalOrder)

        return Array((exact + prefix + rest).prefix(Self.limit))
    }

    private func filteredNets(_ board: LoadedBoard) -> [NetEntry] {
        let q = query.trimmingCharacters(in: .whitespaces)
        let all = board.nets.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }

        guard !q.isEmpty else { return Array(all.prefix(Self.limit)) }

        return Array(all.filter { $0.name.localizedCaseInsensitiveContains(q) }.prefix(Self.limit))
    }

    private static func naturalOrder(_ a: FootprintEntry, _ b: FootprintEntry) -> Bool {
        a.reference.localizedStandardCompare(b.reference) == .orderedAscending
    }
}
