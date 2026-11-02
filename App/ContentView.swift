// Netloom - PCB design for iPad
// Copyright (C) 2026 Seyhan Labs. Licensed under the GNU GPL v3 or later.

import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var store: BoardStore
    @State private var showImporter = false
    @State private var showLayers = false
    @State private var showInfo = false
    @State private var showHelp = false
    @State private var showSearch = false

    var body: some View {
        NavigationStack {
            ZStack(alignment: .bottomLeading) {
                if store.board != nil {
                    BoardCanvas(board: store.board,
                                visibleSlots: store.visibleSlots,
                                hiddenKinds: store.hiddenKinds,
                                flipped: store.flipped,
                                fitToken: store.fitToken,
                                highlight: store.highlight,
                                measureA: store.measureA,
                                measureB: store.measureB,
                                focus: store.focusRequest,
                                dragPreview: store.dragPreview,
                                onTap: { point, tolerance in
                                    store.handleTap(at: point, tolerance: tolerance)
                                },
                                onDragBegin: { point, tolerance in
                                    store.dragCanBegin(at: point, tolerance: tolerance)
                                },
                                onDragSnap: { raw in
                                    store.dragSnapped(raw)
                                },
                                onDragEnd: { raw in
                                    store.dragEnded(raw)
                                },
                                route: store.routeToolConfig,
                                routeBuilder: { points, tolerance, snap in
                                    store.buildRoute(points, tolerance: tolerance, snapRadius: snap)
                                },
                                onRouteFinished: { route in
                                    store.finishRoute(route)
                                },
                                onPencil: {
                                    store.pencilSeen()
                                },
                                drcMarkers: store.drcMarkers,
                                drcFocus: store.drcFocusPosition,
                                demoStroke: store.demoStroke)
                        .ignoresSafeArea(edges: [.bottom, .horizontal])

                    statusBar

                    if let selection = store.selection, !store.measureMode {
                        SelectionCard(selection: selection)
                            .padding(12)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                            .transition(.move(edge: .trailing).combined(with: .opacity))
                    }

                    if store.measureMode {
                        MeasureBar()
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                    }

                    if store.editMode {
                        VStack(spacing: 6) {
                            EditBar()

                            if store.drawTool {
                                RouteToolBar()
                                    .transition(.move(edge: .top).combined(with: .opacity))
                            }
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                        .transition(.move(edge: .top).combined(with: .opacity))
                    }

                    if let toast = store.toast {
                        ToastView(toast: toast)
                            .padding(.bottom, 56)
                            .padding(.horizontal, 16)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                            .transition(.move(edge: .bottom).combined(with: .opacity))
                    }
                } else {
                    WelcomeView(openFile: { showImporter = true })
                }

                if store.isLoading {
                    loadingOverlay
                }
            }
            .animation(.easeOut(duration: 0.2), value: store.selection?.item)
            .animation(.easeOut(duration: 0.2), value: store.editMode)
            .animation(.easeOut(duration: 0.2), value: store.drawTool)
            .animation(.easeOut(duration: 0.2), value: store.toast)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbarContent }
            .fileImporter(isPresented: $showImporter,
                          allowedContentTypes: [.kicadPCB, .kicadSchematic, .zip, .folder, .data],
                          allowsMultipleSelection: true) { result in
                switch result {
                case .success(let urls):
                    store.open(urls: urls)
                case .failure(let error):
                    store.errorMessage = error.localizedDescription
                }
            }
            .sheet(isPresented: $showInfo) {
                if let board = store.board {
                    BoardInfoView(board: board)
                }
            }
            .sheet(isPresented: $showHelp) {
                HelpView()
            }
            .sheet(isPresented: $showSearch) {
                SearchView()
                    .environmentObject(store)
            }
            .sheet(isPresented: $store.showDRC) {
                DRCView()
                    .environmentObject(store)
            }
            .sheet(isPresented: $store.showFab) {
                FabView()
                    .environmentObject(store)
            }
            .sheet(item: $store.activeSheet) { sheet in
                Group {
                    switch sheet {
                    case .fields: FieldEditSheet()
                    case .trackWidth: TrackWidthSheet()
                    case .via: ViaSheet()
                    }
                }
                .environmentObject(store)
                .presentationDetents([.medium, .large])
            }
            .fileExporter(isPresented: $store.showExporter,
                          document: store.exportFile,
                          contentType: .kicadPCB,
                          defaultFilename: store.documentName + ".kicad_pcb") { result in
                store.exportFinished(result)
            }
            .confirmationDialog("You have unsaved changes",
                                isPresented: Binding(get: { store.pendingAction != nil },
                                                     set: { if !$0 { store.pendingAction = nil } }),
                                titleVisibility: .visible) {
                if store.canSaveInPlace {
                    Button("Save and continue") { store.saveAndContinue() }
                }
                Button("Continue without saving", role: .destructive) { store.discardAndContinue() }
                Button("Cancel", role: .cancel) { store.pendingAction = nil }
            } message: {
                Text(store.canSaveInPlace
                     ? "\(store.documentName) has unsaved edits." as LocalizedStringKey
                     : "\(store.documentName) is a sample board; use \"Save as\" first to keep your changes.")
            }
            .task {
                store.handleLaunchArguments()
            }
            .onChange(of: store.autoPanel) { panel in
                guard let panel else { return }
                store.autoPanel = nil

                // let the first frame render before presenting
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                    if panel == "info" {
                        showInfo = true
                    } else if panel == "layers" {
                        showLayers = true
                    } else if panel == "search" {
                        showSearch = true
                    } else if panel == "fields" {
                        store.activeSheet = .fields
                    } else if panel == "help" {
                        showHelp = true
                    }
                }
            }
            .alert("The file could not be opened",
                   isPresented: Binding(get: { store.errorMessage != nil },
                                        set: { if !$0 { store.errorMessage = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(store.errorMessage ?? "")
            }
        }
    }

    private var title: String {
        guard let board = store.board else { return "Netloom" }

        if board.isSchematic, let sheet = board.currentSheet, sheet.id != 0 {
            return "\(board.name) › \(sheet.title)"
        }

        return store.docState.modified ? "\(board.name) •" : board.name
    }

    // MARK: - Toolbar

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .navigationBarLeading) {
            Button {
                showImporter = true
            } label: {
                Label("Open", systemImage: "folder")
            }

            Menu {
                ForEach(store.samples) { sample in
                    Button(sample.title) { store.openSample(sample) }
                }
            } label: {
                Label("Samples", systemImage: "square.grid.2x2")
            }

            if store.project?.hasBoard == true && store.project?.hasSchematic == true {
                Button {
                    store.switchView()
                } label: {
                    Label(store.isSchematicView ? "Board" as LocalizedStringKey : "Schematic",
                          systemImage: store.isSchematicView ? "cpu" : "flowchart")
                }
                .disabled(!store.canSwitchView)
            }

            if let board = store.board, board.isSchematic, board.sheets.count > 1 {
                sheetMenu(board)
            }
        }

        ToolbarItemGroup(placement: .navigationBarTrailing) {
            if store.board != nil {
                if !store.isViewOnly {
                    Button {
                        store.editMode.toggle()
                    } label: {
                        Label(store.editMode ? "Finish editing" as LocalizedStringKey : "Edit",
                              systemImage: store.editMode ? "pencil.circle.fill" : "pencil.circle")
                    }

                    Button {
                        store.showDRC = true
                    } label: {
                        Label("Design check", systemImage: store.drcMarkers.isEmpty ? "checkmark.shield" : "exclamationmark.shield")
                    }

                    Button {
                        store.showFab = true
                    } label: {
                        Label("Fabrication files", systemImage: "shippingbox")
                    }
                }

                Button {
                    showSearch = true
                } label: {
                    Label("Search", systemImage: "magnifyingglass")
                }

                Button {
                    store.measureMode.toggle()
                } label: {
                    Label("Measure", systemImage: store.measureMode ? "ruler.fill" : "ruler")
                }

                if !store.isSchematicView {
                    Button {
                        store.flipped.toggle()
                    } label: {
                        Label(store.flipped ? "View from top" as LocalizedStringKey : "View from bottom",
                              systemImage: store.flipped ? "arrow.up.square" : "arrow.down.square")
                    }
                }

                Button {
                    store.requestFit()
                } label: {
                    Label("Fit", systemImage: "arrow.up.left.and.arrow.down.right")
                }

                Button {
                    showLayers = true
                } label: {
                    Label("Layers", systemImage: "square.3.layers.3d")
                }
                .popover(isPresented: $showLayers) {
                    LayerPanel()
                        .environmentObject(store)
                        .frame(minWidth: 340, idealWidth: 360, minHeight: 520, idealHeight: 700)
                }

                Button {
                    showInfo = true
                } label: {
                    Label("Info", systemImage: "info.circle")
                }
            }

            Button {
                showHelp = true
            } label: {
                Label("Help", systemImage: "questionmark.circle")
            }
        }
    }

    /// Sheets of the schematic, indented by depth, and a way up to the parent sheet.
    @ViewBuilder
    private func sheetMenu(_ board: LoadedBoard) -> some View {
        if let current = board.currentSheet, current.parent >= 0 {
            Button {
                store.openSheet(current.parent)
            } label: {
                Label("Parent sheet", systemImage: "arrow.turn.left.up")
            }
        }

        Menu {
            ForEach(board.sheets) { sheet in
                Button {
                    store.openSheet(sheet.id)
                } label: {
                    let indent = String(repeating: "    ", count: max(0, sheet.depth))
                    let page = sheet.page.isEmpty ? "" : "  (\(sheet.page))"

                    if sheet.id == board.sheetIndex {
                        Label(indent + sheet.title + page, systemImage: "checkmark")
                    } else {
                        Text(indent + sheet.title + page)
                    }
                }
            }
        } label: {
            Label("Sheets", systemImage: "rectangle.stack")
        }
    }

    // MARK: - Overlays

    private var statusBar: some View {
        HStack(spacing: 12) {
            if store.drawTool {
                Image(systemName: "pencil.tip")
                Text(store.fingerDrawing
                     ? "Draw from a pad, via or track end · Two fingers move the board" as LocalizedStringKey
                     : "Draw with Apple Pencil from a pad, via or track end · Fingers move the board")
            } else if store.editMode && store.editFootprint != nil {
                Image(systemName: "hand.draw")
                Text("Drag the footprint to move it · \(gridTitle(store.gridStep))")
            } else if store.editMode {
                Image(systemName: "pencil")
                Text("Editing: select a footprint, track, via, zone or text")
            } else if let board = store.board, board.isSchematic, !board.missingSheetFiles.isEmpty, store.cursorText == nil {
                Image(systemName: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
                Text("Some sub-sheet files are missing: open the project folder to see every sheet")
            } else if let text = store.cursorText {
                Image(systemName: "scope")
                Text(text)
                    .monospacedDigit()
            } else {
                Text("Tap: select · Tap again: item underneath · Double tap: zoom · Two fingers: fit")
            }

            if store.flipped {
                Text("BOTTOM VIEW")
                    .fontWeight(.semibold)
                    .foregroundStyle(.orange)
            }
        }
        .font(.footnote)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
        .padding(12)
    }

    private var loadingOverlay: some View {
        VStack(spacing: 12) {
            ProgressView()
                .controlSize(.large)
            Text("Loading \(store.loadingName)…")
                .font(.headline)
        }
        .padding(24)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Welcome

struct WelcomeView: View {
    @EnvironmentObject private var store: BoardStore
    let openFile: () -> Void

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                Spacer(minLength: 40)

                Image(systemName: "cpu")
                    .font(.system(size: 72, weight: .light))
                    .foregroundStyle(Color(red: 200 / 255, green: 52 / 255, blue: 52 / 255))

                VStack(spacing: 6) {
                    Text("Netloom")
                        .font(.largeTitle.bold())
                    Text("PCB viewer and editor for KiCad files")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                }

                Button(action: openFile) {
                    Label("Open a KiCad file or project folder", systemImage: "folder")
                        .frame(maxWidth: 360)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)

                Text("Boards (.kicad_pcb) and schematics (.kicad_sch) open directly; pick the project folder to get every sheet and switch between board and schematic. Gerber and drill files can be opened too: pick them all at once, their folder or their ZIP.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)

                VStack(alignment: .leading, spacing: 10) {
                    Text("Samples")
                        .font(.headline)

                    ForEach(store.samples) { sample in
                        Button {
                            store.openSample(sample)
                        } label: {
                            Label(sample.title, systemImage: "square.grid.2x2")
                        }
                    }
                }
                .frame(maxWidth: 360, alignment: .leading)
                .padding()
                .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 14))

                Text("Opens KiCad 5 – 10 board files and KiCad 6 – 10 schematics. Pick a file from the Files app, iCloud Drive or AirDrop.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)

                HStack(spacing: 8) {
                    Image("SeyhanLabsLogo")
                        .resizable()
                        .scaledToFit()
                        .frame(width: 28, height: 28)
                    Text(verbatim: "Seyhan Labs")
                        .font(.footnote.weight(.medium))
                        .foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)

                Spacer(minLength: 40)
            }
            .frame(maxWidth: .infinity)
            .padding()
        }
    }
}

// MARK: - Help

struct HelpView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("Gestures") {
                    helpRow("hand.draw", "Drag with one or two fingers", "Pans the board")
                    helpRow("arrow.up.left.and.arrow.down.right", "Pinch", "Zooms in and out")
                    helpRow("hand.tap", "Double tap", "Zooms in 2× where you tap")
                    helpRow("hand.tap.fill", "Two-finger tap", "Fits the board to the screen")
                    helpRow("hand.point.up.left", "Tap", "Selects an item (pad, via, track, footprint, zone…) and shows its details")
                    helpRow("square.3.layers.3d.down.right", "Tap the same spot again", "Steps through overlapping items")
                }

                Section("Tools") {
                    helpRow("point.3.connected.trianglepath.dotted", "Highlight net", "Brightens everything on the selected item's net and dims the rest")
                    helpRow("magnifyingglass", "Search", "Finds a footprint (R12, U3, 10k) or net (GND, SDA) and goes there")
                    helpRow("ruler", "Measure", "Tap two points: distance, ΔX, ΔY and angle. Snaps to pad/via centres")
                    helpRow("number.square", "Pad and net names", "Pad numbers and net names appear as you zoom in")
                }

                Section("Editing") {
                    helpRow("pencil.circle", "Edit", "Opens the edit bar. Tapping a pad selects its footprint")
                    helpRow("hand.draw", "Drag the selected footprint", "Moves it and snaps it to the grid; dragging elsewhere pans the board")
                    helpRow("rotate.left", "Rotate", "Rotates the footprint by 90°")
                    helpRow("trash", "Delete", "Deletes a footprint, track, via, zone, graphic or text")
                    helpRow("character.cursor.ibeam", "Reference / value, track width, via", "Appear in the bar depending on the selection")
                    helpRow("arrow.uturn.left", "Undo / Redo", "Every edit can be undone")
                    helpRow("square.and.arrow.down", "Save", "Writes the file back; only changed items are rewritten, the rest stays byte for byte the same")
                    helpRow("line.diagonal", "Ratsnest", "Thin lines between unconnected pads; can be hidden in the Layers panel")
                    Text("Copper zone fills are not recalculated. After moving parts, refill zones in KiCad (B).")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                Section("Tracks, design check and fabrication") {
                    helpRow("pencil.tip.crop.circle", "Draw tracks", "In the edit bar. Draw with Apple Pencil from a pad, via or track end: the line becomes a clean 45° track that locks onto the copper")
                    helpRow("square.3.layers.3d", "Layer, width and via", "Chosen in the drawing bar. Starting or ending on a track of another layer adds a via")
                    helpRow("hand.raised", "Shorts are refused", "A track that would touch another net is not added; red rings show where")
                    helpRow("checkmark.shield", "Design check", "Clearances, shorts, holes, board edge, courtyards and missing connections, with the rules of the KiCad project file when available")
                    helpRow("shippingbox", "Fabrication files", "Gerber, drill, bill of materials and placement files in one ZIP for any board house")
                    helpRow("doc.viewfinder", "Viewing Gerber files", "Open the Gerber and drill files of any CAD program (several at once, their folder or a ZIP) to look at them layer by layer before ordering")
                }

                Section("Schematics") {
                    helpRow("folder", "Open the project folder", "Brings every sheet of a schematic; a single .kicad_sch file shows only its own sheet")
                    helpRow("rectangle.stack", "Sheets", "Lists the sheets of the hierarchy; \"Parent sheet\" goes one level up")
                    helpRow("square.on.square", "Tap a sheet box", "\"Open sheet\" in its card goes into that sheet")
                    helpRow("point.3.connected.trianglepath.dotted", "Highlight net", "Shows the net on this sheet; \"Other sheets\" jumps to where it continues")
                    helpRow("magnifyingglass", "Search", "Finds symbols and nets on every sheet")
                    helpRow("flowchart", "Board / Schematic", "Switches between the board and the schematic of a project; board edits are kept")
                }

                Section("Keyboard / trackpad") {
                    helpRow("rectangle.and.hand.point.up.left", "Scroll on the trackpad", "Pans the board")
                    helpRow("plus.magnifyingglass", "Pinch on the trackpad", "Zooms in")
                }

                Section("Layers") {
                    Text("In the Layers panel you can turn layers and object types (tracks, vias, pads, zones, texts, holes) on and off. \"View from bottom\" shows the board from the back.")
                }

                Section("About") {
                    HStack(spacing: 14) {
                        Image("SeyhanLabsLogo")
                            .resizable()
                            .scaledToFit()
                            .frame(width: 64, height: 64)
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(verbatim: "Netloom")
                                .font(.headline)
                            Text(verbatim: "Seyhan Labs")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 4)
                    Text("Netloom by Seyhan Labs opens and edits KiCad .kicad_pcb files and shows .kicad_sch schematics. It is an independent app, not affiliated with the KiCad project. The public source code is licensed under GPL-3.0.")
                    Text("Core version \(String(cString: kp_core_version()))")
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Help")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
    }

    private func helpRow(_ icon: String, _ title: LocalizedStringKey, _ detail: LocalizedStringKey) -> some View {
        HStack(spacing: 14) {
            Image(systemName: icon)
                .frame(width: 28)
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(detail)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
