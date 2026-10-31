// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

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
                                onTap: { point, tolerance in
                                    store.handleTap(at: point, tolerance: tolerance)
                                })
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
                } else {
                    WelcomeView(openFile: { showImporter = true })
                }

                if store.isLoading {
                    loadingOverlay
                }
            }
            .animation(.easeOut(duration: 0.2), value: store.selection?.item)
            .navigationTitle(store.board?.name ?? "KiPad")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbarContent }
            .fileImporter(isPresented: $showImporter,
                          allowedContentTypes: [.kicadPCB, .data],
                          allowsMultipleSelection: false) { result in
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        store.open(url: url)
                    }
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
                    }
                }
            }
            .alert("Dosya açılamadı",
                   isPresented: Binding(get: { store.errorMessage != nil },
                                        set: { if !$0 { store.errorMessage = nil } })) {
                Button("Tamam", role: .cancel) {}
            } message: {
                Text(store.errorMessage ?? "")
            }
        }
    }

    // MARK: - Toolbar

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .navigationBarLeading) {
            Button {
                showImporter = true
            } label: {
                Label("Aç", systemImage: "folder")
            }

            Menu {
                ForEach(store.samples) { sample in
                    Button(sample.title) { store.openSample(sample) }
                }
            } label: {
                Label("Örnekler", systemImage: "square.grid.2x2")
            }
        }

        ToolbarItemGroup(placement: .navigationBarTrailing) {
            if store.board != nil {
                Button {
                    showSearch = true
                } label: {
                    Label("Ara", systemImage: "magnifyingglass")
                }

                Button {
                    store.measureMode.toggle()
                } label: {
                    Label("Ölç", systemImage: store.measureMode ? "ruler.fill" : "ruler")
                }

                Button {
                    store.flipped.toggle()
                } label: {
                    Label(store.flipped ? "Üstten bak" : "Alttan bak",
                          systemImage: store.flipped ? "arrow.up.square" : "arrow.down.square")
                }

                Button {
                    store.requestFit()
                } label: {
                    Label("Sığdır", systemImage: "arrow.up.left.and.arrow.down.right")
                }

                Button {
                    showLayers = true
                } label: {
                    Label("Katmanlar", systemImage: "square.3.layers.3d")
                }
                .popover(isPresented: $showLayers) {
                    LayerPanel()
                        .environmentObject(store)
                        .frame(minWidth: 340, idealWidth: 360, minHeight: 520, idealHeight: 700)
                }

                Button {
                    showInfo = true
                } label: {
                    Label("Bilgi", systemImage: "info.circle")
                }
            }

            Button {
                showHelp = true
            } label: {
                Label("Yardım", systemImage: "questionmark.circle")
            }
        }
    }

    // MARK: - Overlays

    private var statusBar: some View {
        HStack(spacing: 12) {
            if let text = store.cursorText {
                Image(systemName: "scope")
                Text(text)
                    .monospacedDigit()
            } else {
                Text("Dokun: seç · Aynı yere tekrar dokun: alttaki öğe · Çift dokun: yakınlaş · İki parmak: sığdır")
            }

            if store.flipped {
                Text("ALTTAN GÖRÜNÜM")
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
            Text("\(store.loadingName) yükleniyor…")
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
                    Text("KiPad")
                        .font(.largeTitle.bold())
                    Text("KiCad kart görüntüleyici")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                }

                Button(action: openFile) {
                    Label("Kart dosyası aç (.kicad_pcb)", systemImage: "folder")
                        .frame(maxWidth: 360)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)

                VStack(alignment: .leading, spacing: 10) {
                    Text("Örnek kartlar")
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

                Text("KiCad 5 – 10 kart dosyalarını açar. Dosyayı Dosyalar uygulamasından, iCloud Drive'dan "
                     + "ya da AirDrop ile seçebilirsin.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)

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
                Section("Hareketler") {
                    helpRow("hand.draw", "Tek ya da iki parmakla sürükle", "Kartı kaydırır")
                    helpRow("arrow.up.left.and.arrow.down.right", "İki parmakla sıkıştır", "Yakınlaştırır / uzaklaştırır")
                    helpRow("hand.tap", "Çift dokun", "Dokunulan yere 2× yakınlaşır")
                    helpRow("hand.tap.fill", "İki parmakla tek dokun", "Kartı ekrana sığdırır")
                    helpRow("hand.point.up.left", "Tek dokun", "Öğeyi seçer (pad, via, pist, footprint, bölge…) ve bilgisini gösterir")
                    helpRow("square.3.layers.3d.down.right", "Aynı yere tekrar dokun", "Üst üste binen öğeler arasında geçer")
                }

                Section("Araçlar") {
                    helpRow("point.3.connected.trianglepath.dotted", "Neti vurgula", "Seçili öğenin netindeki her şeyi parlatır, gerisini karartır")
                    helpRow("magnifyingglass", "Ara", "Footprint (R12, U3, 10k) ya da net (GND, SDA) bulur ve oraya gider")
                    helpRow("ruler", "Ölç", "İki noktaya dokun: mesafe, ΔX, ΔY ve açı. Pad/via merkezlerine tutunur")
                    helpRow("number.square", "Pad ve net adları", "Yakınlaştıkça pad numaraları ve net adları görünür")
                }

                Section("Klavye / trackpad") {
                    helpRow("rectangle.and.hand.point.up.left", "Trackpad ile kaydır", "Kartı kaydırır")
                    helpRow("plus.magnifyingglass", "Trackpad ile sıkıştır", "Yakınlaştırır")
                }

                Section("Katmanlar") {
                    Text("Katmanlar panelinden katmanları ve nesne türlerini (pist, via, pad, bölge, yazı, delik) "
                         + "tek tek açıp kapatabilirsin. \"Alttan bak\" kartı arka yüzden gösterir.")
                }

                Section("Hakkında") {
                    Text("KiPad, KiCad'in .kicad_pcb dosyalarını okuyan bağımsız bir görüntüleyicidir. "
                         + "KiCad'in stroke yazı tipini ve renk temasını kullanır. GPL-3.0 lisanslıdır.")
                    Text("Çekirdek sürümü \(String(cString: kp_core_version()))")
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Yardım")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Kapat") { dismiss() }
                }
            }
        }
    }

    private func helpRow(_ icon: String, _ title: String, _ detail: String) -> some View {
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
