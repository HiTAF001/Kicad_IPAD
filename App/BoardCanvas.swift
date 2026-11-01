// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.

import MetalKit
import SwiftUI

/// SwiftUI wrapper around the Metal board view.
struct BoardCanvas: UIViewRepresentable {
    let board: LoadedBoard?
    let visibleSlots: Set<Int>
    let hiddenKinds: Set<Int>
    let flipped: Bool
    let fitToken: Int
    let highlight: HighlightSpec?
    let measureA: SIMD2<Double>?
    let measureB: SIMD2<Double>?
    let focus: FocusRequest?
    /// Offset of a footprint that was just moved, shown until the rebuilt board arrives.
    let dragPreview: SIMD2<Double>?
    /// World position (mm) and finger tolerance (mm).
    let onTap: @MainActor (SIMD2<Double>, Double) -> Void
    let onDragBegin: @MainActor (SIMD2<Double>, Double) -> Bool
    let onDragSnap: @MainActor (SIMD2<Double>) -> SIMD2<Double>
    let onDragEnd: @MainActor (SIMD2<Double>) -> Void

    func makeCoordinator() -> BoardRenderer {
        BoardRenderer()
    }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MetalContext.shared.device)
        context.coordinator.attach(to: view)
        return view
    }

    func updateUIView(_ view: MTKView, context: Context) {
        let renderer = context.coordinator
        renderer.onTap = onTap
        renderer.onDragBegin = onDragBegin
        renderer.onDragSnap = onDragSnap
        renderer.onDragEnd = onDragEnd
        renderer.update(board: board,
                        visibleSlots: visibleSlots,
                        hiddenKinds: hiddenKinds,
                        flipped: flipped,
                        fitToken: fitToken,
                        highlight: highlight,
                        measureA: measureA,
                        measureB: measureB,
                        focus: focus,
                        dragPreview: dragPreview)
    }
}
