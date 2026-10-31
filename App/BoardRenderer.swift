// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.
//
// Metal renderer and touch handling for the board canvas.
//
// Every draw group is a set of triangles of one colour. Layers are drawn back to front; within
// one layer slot a stencil test (reference = slot + 1, "not equal" then "replace") makes sure
// each pixel is painted only once, so translucent layers (solder mask, paste) do not get darker
// where shapes overlap - the same look KiCad has.
//
// Highlighting: a full-screen quad dims the board (and resets the stencil), then the selected
// item's triangles - gathered from the main index buffer - are drawn again in a brighter colour.

import MetalKit
import UIKit
import simd

private struct Uniforms {
    var scale: SIMD2<Float>
    var offset: SIMD2<Float>
    var color: SIMD4<Float>
}

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float2 scale;
    float2 offset;
    float4 color;
};

vertex float4 kp_vertex(const device float2* positions [[buffer(0)]],
                        constant Uniforms& u [[buffer(1)]],
                        uint vid [[vertex_id]])
{
    float2 p = positions[vid] * u.scale + u.offset;
    return float4(p, 0.0, 1.0);
}

fragment float4 kp_fragment(constant Uniforms& u [[buffer(1)]])
{
    // premultiplied alpha
    return float4(u.color.rgb * u.color.a, u.color.a);
}
"""

private struct DrawItem {
    let stencilRef: UInt32
    let color: SIMD4<Float>
    let indexOffset: Int
    let indexCount: Int
    let minZoom: Double
}

private let backgroundColor = SIMD4<Float>(0, 16.0 / 255.0, 35.0 / 255.0, 1)

@MainActor
final class BoardRenderer: NSObject, MTKViewDelegate, UIGestureRecognizerDelegate {
    private let device: MTLDevice?
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState?
    private var stencilState: MTLDepthStencilState?
    private var resetState: MTLDepthStencilState?
    private var overlayState: MTLDepthStencilState?
    private var quadBuffer: MTLBuffer?
    private weak var view: MTKView?

    private var board: LoadedBoard?
    private var visibleSlots: Set<Int> = []
    private var hiddenKinds: Set<Int> = []
    private var flipped = false
    private var lastFitToken = -1
    private var drawList: [DrawItem] = []

    private var highlight: HighlightSpec?
    private var highlightBuffer: MTLBuffer?
    private var highlightList: [DrawItem] = []

    private var measureA: SIMD2<Double>?
    private var measureB: SIMD2<Double>?
    private var lastFocusID = 0

    // Camera: world point at the view centre and drawable pixels per millimetre.
    private var center = SIMD2<Double>(0, 0)
    private var zoom: Double = 1
    private var fitZoom: Double = 1
    private var needsFit = true
    private var pendingFocus: FocusRequest?

    var onTap: (@MainActor (SIMD2<Double>, Double) -> Void)?

    override init() {
        device = MetalContext.shared.device
        super.init()
    }

    // MARK: - Setup

    func attach(to view: MTKView) {
        self.view = view
        view.device = device
        view.delegate = self
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .stencil8
        view.sampleCount = (device?.supportsTextureSampleCount(4) ?? false) ? 4 : 1
        view.clearColor = MTLClearColor(red: 0, green: 16.0 / 255.0, blue: 35.0 / 255.0, alpha: 1)
        view.clearStencil = 0
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = true
        view.isMultipleTouchEnabled = true
        view.backgroundColor = UIColor(red: 0, green: 16.0 / 255.0, blue: 35.0 / 255.0, alpha: 1)

        buildPipeline(for: view)
        addGestures(to: view)
    }

    private func buildPipeline(for view: MTKView) {
        guard let device else { return }

        queue = device.makeCommandQueue()

        do {
            let library = try device.makeLibrary(source: shaderSource, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.label = "KiPad board"
            desc.vertexFunction = library.makeFunction(name: "kp_vertex")
            desc.fragmentFunction = library.makeFunction(name: "kp_fragment")
            desc.rasterSampleCount = view.sampleCount
            desc.stencilAttachmentPixelFormat = view.depthStencilPixelFormat

            let color = desc.colorAttachments[0]!
            color.pixelFormat = view.colorPixelFormat
            color.isBlendingEnabled = true
            color.rgbBlendOperation = .add
            color.alphaBlendOperation = .add
            color.sourceRGBBlendFactor = .one
            color.sourceAlphaBlendFactor = .one
            color.destinationRGBBlendFactor = .oneMinusSourceAlpha
            color.destinationAlphaBlendFactor = .oneMinusSourceAlpha

            pipeline = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("KiPad: pipeline error: \(error)")
        }

        func makeState(compare: MTLCompareFunction, pass: MTLStencilOperation) -> MTLDepthStencilState? {
            let stencil = MTLStencilDescriptor()
            stencil.stencilCompareFunction = compare
            stencil.stencilFailureOperation = .keep
            stencil.depthFailureOperation = .keep
            stencil.depthStencilPassOperation = pass
            stencil.readMask = 0xFF
            stencil.writeMask = 0xFF

            let ds = MTLDepthStencilDescriptor()
            ds.depthCompareFunction = .always
            ds.isDepthWriteEnabled = false
            ds.frontFaceStencil = stencil
            ds.backFaceStencil = stencil
            return device.makeDepthStencilState(descriptor: ds)
        }

        stencilState = makeState(compare: .notEqual, pass: .replace) // paint each pixel once per slot
        resetState = makeState(compare: .always, pass: .replace)     // dim quad: stencil := 0
        overlayState = makeState(compare: .always, pass: .keep)      // measuring overlay

        let quad: [SIMD2<Float>] = [SIMD2(-1, -1), SIMD2(1, -1), SIMD2(-1, 1), SIMD2(1, 1)]
        quadBuffer = device.makeBuffer(bytes: quad, length: MemoryLayout<SIMD2<Float>>.stride * quad.count,
                                       options: .storageModeShared)
    }

    private func addGestures(to view: MTKView) {
        let pan = UIPanGestureRecognizer(target: self, action: #selector(handlePan(_:)))
        pan.maximumNumberOfTouches = 2
        pan.allowedScrollTypesMask = .all // trackpad / mouse wheel scrolling pans
        pan.delegate = self
        view.addGestureRecognizer(pan)

        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(handlePinch(_:)))
        pinch.delegate = self
        view.addGestureRecognizer(pinch)

        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        view.addGestureRecognizer(doubleTap)

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleTwoFingerTap(_:)))
        twoFingerTap.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFingerTap)

        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap(_:)))
        tap.require(toFail: doubleTap)
        view.addGestureRecognizer(tap)
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        true
    }

    // MARK: - State from SwiftUI

    func update(board: LoadedBoard?, visibleSlots: Set<Int>, hiddenKinds: Set<Int>, flipped: Bool,
                fitToken: Int, highlight: HighlightSpec?, measureA: SIMD2<Double>?,
                measureB: SIMD2<Double>?, focus: FocusRequest?) {
        var dirty = false
        var highlightDirty = false

        if board !== self.board {
            self.board = board
            needsFit = true
            dirty = true
            highlightDirty = true
        }

        if visibleSlots != self.visibleSlots || hiddenKinds != self.hiddenKinds {
            self.visibleSlots = visibleSlots
            self.hiddenKinds = hiddenKinds
            dirty = true
            highlightDirty = true
        }

        if flipped != self.flipped {
            self.flipped = flipped
            dirty = true
            highlightDirty = true
        }

        if fitToken != lastFitToken {
            lastFitToken = fitToken
            needsFit = true
            dirty = true
        }

        if highlight != self.highlight {
            self.highlight = highlight
            highlightDirty = true
        }

        if measureA != self.measureA || measureB != self.measureB {
            self.measureA = measureA
            self.measureB = measureB
            requestDraw()
        }

        if let focus, focus.id != lastFocusID {
            lastFocusID = focus.id
            pendingFocus = focus
            requestDraw()
        }

        if dirty {
            rebuildDrawList()
        }

        if highlightDirty {
            rebuildHighlight()
        }

        if dirty || highlightDirty {
            requestDraw()
        }
    }

    private func sortKey(_ slot: Int) -> Int {
        guard let board, slot >= 0, slot < board.slots.count else { return 0 }
        return flipped ? board.slots[slot].sortBack : board.slots[slot].sortFront
    }

    private func slotShown(_ slot: Int) -> Bool {
        guard let board, slot >= 0, slot < board.slots.count else { return false }

        let s = board.slots[slot]

        if s.parentSlot >= 0 {
            // label slot: follows its copper layer and the current view side
            if !visibleSlots.contains(s.parentSlot) {
                return false
            }

            if s.viewSide != 0 && s.viewSide != (flipped ? -1 : 1) {
                return false
            }

            return true
        }

        return visibleSlots.contains(slot)
    }

    private func rebuildDrawList() {
        guard let board else {
            drawList = []
            return
        }

        let ordered = board.groups.enumerated().sorted { a, b in
            let ka = sortKey(a.element.slot)
            let kb = sortKey(b.element.slot)
            return ka != kb ? ka < kb : a.offset < b.offset
        }

        drawList = ordered.compactMap { pair -> DrawItem? in
            let g = pair.element

            guard slotShown(g.slot), !hiddenKinds.contains(g.kind), g.indexCount > 0 else {
                return nil
            }

            return DrawItem(stencilRef: UInt32(g.slot % 255 + 1),
                            color: g.color,
                            indexOffset: Int(g.indexStart) * MemoryLayout<UInt32>.stride,
                            indexCount: Int(g.indexCount),
                            minZoom: Double(g.minZoom))
        }
    }

    private func rebuildHighlight() {
        highlightBuffer = nil
        highlightList = []

        guard let board, let highlight, let ib = board.indexBuffer, let device else { return }

        let ranges = board.ranges(of: highlight.item, wholeNet: highlight.wholeNet).filter {
            let slot = Int($0.slot)
            return slotShown(slot) && !hiddenKinds.contains(Int($0.kind))
        }

        guard !ranges.isEmpty else { return }

        let sorted = ranges.enumerated().sorted { a, b in
            let ka = sortKey(Int(a.element.slot))
            let kb = sortKey(Int(b.element.slot))
            return ka != kb ? ka < kb : a.offset < b.offset
        }.map { $0.element }

        let available = ib.length / MemoryLayout<UInt32>.stride
        let source = ib.contents().bindMemory(to: UInt32.self, capacity: available)
        var indices: [UInt32] = []
        indices.reserveCapacity(sorted.reduce(0) { $0 + Int($1.index_count) })

        var items: [DrawItem] = []
        var runSlot = -1
        var runStart = 0

        func closeRun() {
            guard runSlot >= 0, indices.count > runStart else { return }

            let base = board.slots[runSlot].color
            let mix: Float = 0.45
            let bright = SIMD4<Float>(base.x + (1 - base.x) * mix,
                                      base.y + (1 - base.y) * mix,
                                      base.z + (1 - base.z) * mix,
                                      max(base.w, 0.85))
            items.append(DrawItem(stencilRef: UInt32(runSlot % 255 + 1),
                                  color: bright,
                                  indexOffset: runStart * MemoryLayout<UInt32>.stride,
                                  indexCount: indices.count - runStart,
                                  minZoom: 0))
        }

        for r in sorted {
            let start = Int(r.index_start)
            let count = Int(r.index_count)

            guard start >= 0, count > 0, start + count <= available else { continue }

            if Int(r.slot) != runSlot {
                closeRun()
                runSlot = Int(r.slot)
                runStart = indices.count
            }

            indices.append(contentsOf: UnsafeBufferPointer(start: source + start, count: count))
        }

        closeRun()

        guard !indices.isEmpty else { return }

        highlightBuffer = device.makeBuffer(bytes: indices,
                                            length: indices.count * MemoryLayout<UInt32>.stride,
                                            options: .storageModeShared)
        highlightList = items
    }

    private func requestDraw() {
        view?.setNeedsDisplay()
    }

    // MARK: - Camera

    private var flipSign: Double { flipped ? -1 : 1 }

    private func fit(drawableSize size: CGSize) {
        guard let board, size.width > 0, size.height > 0 else { return }

        let bw = max(board.bounds.width, 1.0)
        let bh = max(board.bounds.height, 1.0)
        zoom = min(Double(size.width) / (bw * 1.08), Double(size.height) / (bh * 1.08))
        fitZoom = zoom
        center = SIMD2(board.bounds.midX, board.bounds.midY)
        needsFit = false
    }

    private func applyFocus(_ request: FocusRequest, drawableSize size: CGSize) {
        guard size.width > 0, size.height > 0 else { return }

        let b = request.bounds
        let bw = max(b.width, 0.5)
        let bh = max(b.height, 0.5)

        // Keep the right part of a wide screen for the selection card.
        let scale = Double(view?.contentScaleFactor ?? 2)
        var reserve = 0.0

        if request.reserveRight && Double(size.width) > 700 * scale {
            reserve = min(410 * scale, Double(size.width) * 0.42)
        }

        let availW = Double(size.width) - reserve

        // small items get room around them; big ones (a whole net) nearly fill the view
        var factor = 2.2

        if let board {
            let rel = max(bw / max(board.bounds.width, 1), bh / max(board.bounds.height, 1))
            factor = rel > 0.35 ? 1.1 : 2.2
        }

        zoom = clampZoom(min(availW / (bw * factor), Double(size.height) / (bh * factor)))
        // put the item in the middle of the free area
        center = SIMD2(b.midX + (reserve / 2) / zoom * flipSign, b.midY)
    }

    private func clampZoom(_ z: Double) -> Double {
        min(max(z, fitZoom / 50), 100_000)
    }

    private func screenToWorld(_ p: CGPoint, in view: UIView) -> SIMD2<Double> {
        let s = Double(view.contentScaleFactor)
        let w = Double(view.bounds.width) * s
        let h = Double(view.bounds.height) * s
        let px = Double(p.x) * s - w / 2
        let py = Double(p.y) * s - h / 2
        return SIMD2(center.x + px / zoom * flipSign, center.y + py / zoom)
    }

    // MARK: - Gestures

    @objc private func handlePan(_ g: UIPanGestureRecognizer) {
        guard let view = g.view else { return }

        let t = g.translation(in: view)
        g.setTranslation(.zero, in: view)
        let s = Double(view.contentScaleFactor)
        center.x -= Double(t.x) * s / zoom * flipSign
        center.y -= Double(t.y) * s / zoom
        requestDraw()
    }

    @objc private func handlePinch(_ g: UIPinchGestureRecognizer) {
        guard let view = g.view, g.state == .began || g.state == .changed else { return }

        let loc = g.location(in: view)
        let before = screenToWorld(loc, in: view)
        zoom = clampZoom(zoom * Double(g.scale))
        g.scale = 1
        let after = screenToWorld(loc, in: view)
        center += before - after
        requestDraw()
    }

    @objc private func handleDoubleTap(_ g: UITapGestureRecognizer) {
        guard let view = g.view else { return }

        // zoom in 2x around the tap
        let loc = g.location(in: view)
        let before = screenToWorld(loc, in: view)
        zoom = clampZoom(zoom * 2)
        let after = screenToWorld(loc, in: view)
        center += before - after
        requestDraw()
    }

    @objc private func handleTwoFingerTap(_ g: UITapGestureRecognizer) {
        needsFit = true
        requestDraw()
    }

    @objc private func handleTap(_ g: UITapGestureRecognizer) {
        guard let view = g.view else { return }

        // ~12 points of finger tolerance, in millimetres
        let tolerance = 12.0 * Double(view.contentScaleFactor) / zoom
        onTap?(screenToWorld(g.location(in: view), in: view), tolerance)
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        if needsFit {
            fit(drawableSize: size)
        }

        view.setNeedsDisplay()
    }

    func draw(in view: MTKView) {
        let size = view.drawableSize

        if needsFit {
            fit(drawableSize: size)
        }

        if let focus = pendingFocus, size.width > 0 {
            pendingFocus = nil
            applyFocus(focus, drawableSize: size)
        }

        guard let queue, let pipeline, let stencilState,
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else {
            return
        }

        if let board, let vb = board.vertexBuffer, let ib = board.indexBuffer,
           size.width > 0, size.height > 0 {
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(stencilState)
            encoder.setCullMode(.none)
            encoder.setVertexBuffer(vb, offset: 0, index: 0)

            let sx = zoom / (Double(size.width) / 2) * flipSign
            let sy = -zoom / (Double(size.height) / 2)
            var u = Uniforms(scale: SIMD2(Float(sx), Float(sy)),
                             offset: SIMD2(Float(-center.x * sx), Float(-center.y * sy)),
                             color: .zero)

            for item in drawList where zoom >= item.minZoom {
                u.color = item.color
                encoder.setStencilReferenceValue(item.stencilRef)
                encoder.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                encoder.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                encoder.drawIndexedPrimitives(type: .triangle,
                                              indexCount: item.indexCount,
                                              indexType: .uint32,
                                              indexBuffer: ib,
                                              indexBufferOffset: item.indexOffset)
            }

            // ---- highlight: dim everything, then redraw the selection brighter
            if let hb = highlightBuffer, !highlightList.isEmpty, let quad = quadBuffer, let resetState {
                var dim = Uniforms(scale: SIMD2(1, 1), offset: SIMD2(0, 0),
                                   color: SIMD4(backgroundColor.x, backgroundColor.y, backgroundColor.z, 0.62))
                encoder.setDepthStencilState(resetState)
                encoder.setStencilReferenceValue(0)
                encoder.setVertexBuffer(quad, offset: 0, index: 0)
                encoder.setVertexBytes(&dim, length: MemoryLayout<Uniforms>.stride, index: 1)
                encoder.setFragmentBytes(&dim, length: MemoryLayout<Uniforms>.stride, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)

                encoder.setDepthStencilState(stencilState)
                encoder.setVertexBuffer(vb, offset: 0, index: 0)

                for item in highlightList {
                    u.color = item.color
                    encoder.setStencilReferenceValue(item.stencilRef)
                    encoder.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                    encoder.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                    encoder.drawIndexedPrimitives(type: .triangle,
                                                  indexCount: item.indexCount,
                                                  indexType: .uint32,
                                                  indexBuffer: hb,
                                                  indexBufferOffset: item.indexOffset)
                }
            }

            // ---- measuring overlay (constant size on screen)
            if let a = measureA, let overlayState, let device {
                let px = 1.0 / zoom // one drawable pixel in millimetres
                var dark: [SIMD2<Float>] = []
                var bright: [SIMD2<Float>] = []

                if let b = measureB {
                    appendSegment(&dark, a, b, width: 6 * px)
                    appendSegment(&bright, a, b, width: 3 * px)
                }

                for p in [measureA, measureB].compactMap({ $0 }) {
                    appendDisc(&dark, p, radius: 9 * px)
                    appendDisc(&bright, p, radius: 6 * px)
                    appendDisc(&dark, p, radius: 2.5 * px)
                }

                _ = a
                encoder.setDepthStencilState(overlayState)

                for (verts, color) in [(dark, SIMD4<Float>(0, 0, 0, 0.85)),
                                       (bright, SIMD4<Float>(1, 0.84, 0.04, 1))] where !verts.isEmpty {
                    guard let buffer = device.makeBuffer(bytes: verts,
                                                         length: verts.count * MemoryLayout<SIMD2<Float>>.stride,
                                                         options: .storageModeShared) else { continue }
                    u.color = color
                    encoder.setVertexBuffer(buffer, offset: 0, index: 0)
                    encoder.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                    encoder.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: verts.count)
                }
            }
        }

        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }

    // MARK: - Overlay geometry (world coordinates)

    private func appendSegment(_ out: inout [SIMD2<Float>], _ a: SIMD2<Double>, _ b: SIMD2<Double>,
                               width: Double) {
        let d = b - a
        let len = simd_length(d)

        guard len > 0 else { return }

        let n = SIMD2(-d.y, d.x) / len * (width / 2)
        let p0 = SIMD2<Float>(a + n), p1 = SIMD2<Float>(a - n)
        let p2 = SIMD2<Float>(b - n), p3 = SIMD2<Float>(b + n)
        out.append(contentsOf: [p0, p1, p2, p0, p2, p3])
    }

    private func appendDisc(_ out: inout [SIMD2<Float>], _ c: SIMD2<Double>, radius: Double) {
        let segments = 24
        let center = SIMD2<Float>(c)

        for i in 0..<segments {
            let a0 = Double(i) / Double(segments) * 2 * .pi
            let a1 = Double(i + 1) / Double(segments) * 2 * .pi
            out.append(center)
            out.append(SIMD2<Float>(c + SIMD2(cos(a0), sin(a0)) * radius))
            out.append(SIMD2<Float>(c + SIMD2(cos(a1), sin(a1)) * radius))
        }
    }
}
