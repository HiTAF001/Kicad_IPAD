// KiPad - KiCad board viewer for iPad
// Copyright (C) 2026 KiPad contributors. Licensed under the GNU GPL v3 or later.
//
// Metal renderer and touch handling for the board canvas.
//
// Every draw group is a set of triangles of one colour. Layers are drawn back to front; within
// one layer slot a stencil test (reference = slot + 1, "not equal" then "replace") makes sure
// each pixel is painted only once, so translucent layers (solder mask, paste) do not get darker
// where shapes overlap - the same look KiCad has.

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
}

@MainActor
final class BoardRenderer: NSObject, MTKViewDelegate, UIGestureRecognizerDelegate {
    private let device: MTLDevice?
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState?
    private var stencilState: MTLDepthStencilState?
    private weak var view: MTKView?

    private var board: LoadedBoard?
    private var visibleSlots: Set<Int> = []
    private var hiddenKinds: Set<Int> = []
    private var flipped = false
    private var lastFitToken = -1
    private var drawList: [DrawItem] = []

    // Camera: world point at the view centre and drawable pixels per millimetre.
    private var center = SIMD2<Double>(0, 0)
    private var zoom: Double = 1
    private var fitZoom: Double = 1
    private var needsFit = true

    var onTap: (@MainActor (SIMD2<Double>) -> Void)?

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

        let stencil = MTLStencilDescriptor()
        stencil.stencilCompareFunction = .notEqual
        stencil.stencilFailureOperation = .keep
        stencil.depthFailureOperation = .keep
        stencil.depthStencilPassOperation = .replace
        stencil.readMask = 0xFF
        stencil.writeMask = 0xFF

        let ds = MTLDepthStencilDescriptor()
        ds.depthCompareFunction = .always
        ds.isDepthWriteEnabled = false
        ds.frontFaceStencil = stencil
        ds.backFaceStencil = stencil
        stencilState = device.makeDepthStencilState(descriptor: ds)
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

    func update(board: LoadedBoard?, visibleSlots: Set<Int>, hiddenKinds: Set<Int>,
                flipped: Bool, fitToken: Int) {
        var dirty = false

        if board !== self.board {
            self.board = board
            needsFit = true
            dirty = true
        }

        if visibleSlots != self.visibleSlots || hiddenKinds != self.hiddenKinds {
            self.visibleSlots = visibleSlots
            self.hiddenKinds = hiddenKinds
            dirty = true
        }

        if flipped != self.flipped {
            self.flipped = flipped
            dirty = true
        }

        if fitToken != lastFitToken {
            lastFitToken = fitToken
            needsFit = true
            dirty = true
        }

        if dirty {
            rebuildDrawList()
            requestDraw()
        }
    }

    private func rebuildDrawList() {
        guard let board else {
            drawList = []
            return
        }

        let slots = board.slots
        let ordered = board.groups.enumerated().sorted { a, b in
            let ka = flipped ? slots[a.element.slot].sortBack : slots[a.element.slot].sortFront
            let kb = flipped ? slots[b.element.slot].sortBack : slots[b.element.slot].sortFront
            return ka != kb ? ka < kb : a.offset < b.offset
        }

        drawList = ordered.compactMap { pair -> DrawItem? in
            let g = pair.element

            guard visibleSlots.contains(g.slot), !hiddenKinds.contains(g.kind), g.indexCount > 0 else {
                return nil
            }

            return DrawItem(stencilRef: UInt32(g.slot % 255 + 1),
                            color: g.color,
                            indexOffset: Int(g.indexStart) * MemoryLayout<UInt32>.stride,
                            indexCount: Int(g.indexCount))
        }
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
        onTap?(screenToWorld(g.location(in: view), in: view))
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

            for item in drawList {
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
        }

        encoder.endEncoding()
        commands.present(drawable)
        commands.commit()
    }
}
