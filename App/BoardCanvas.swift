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
    let onTap: @MainActor (SIMD2<Double>) -> Void

    func makeCoordinator() -> BoardRenderer {
        BoardRenderer()
    }

    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MetalContext.shared.device)
        context.coordinator.attach(to: view)
        return view
    }

    func updateUIView(_ view: MTKView, context: Context) {
        context.coordinator.onTap = onTap
        context.coordinator.update(board: board,
                                   visibleSlots: visibleSlots,
                                   hiddenKinds: hiddenKinds,
                                   flipped: flipped,
                                   fitToken: fitToken)
    }
}
