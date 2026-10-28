import Metal
import MetalKit

enum RendererError: Error {
    case noCommandQueue
}

// Clears to a slowly cycling colour and draws a rotating RGB triangle. The shader is compiled at runtime
// from this source string (no .metallib build step).
final class Renderer: NSObject, MTKViewDelegate {
    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct Uniforms { float time; float aspect; };
    struct VOut { float4 position [[position]]; float4 color; };

    vertex VOut vs_main(uint vid [[vertex_id]], constant Uniforms &u [[buffer(0)]]) {
        const float2 p[3] = { float2(0.0, 0.6), float2(-0.6, -0.5), float2(0.6, -0.5) };
        const float4 c[3] = { float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1) };
        float s = sin(u.time), co = cos(u.time);
        float2 q = p[vid];
        float2 r = float2(q.x * co - q.y * s, q.x * s + q.y * co);
        r.x /= u.aspect;
        VOut o;
        o.position = float4(r, 0, 1);
        o.color = c[vid];
        return o;
    }

    fragment float4 fs_main(VOut in [[stage_in]]) {
        return in.color;
    }
    """

    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let start = Date()
    private(set) var frameCount = 0
    var onFrame: ((Int) -> Void)?

    init(device: MTLDevice, pixelFormat: MTLPixelFormat) throws {
        guard let q = device.makeCommandQueue() else { throw RendererError.noCommandQueue }
        queue = q
        let library = try device.makeLibrary(source: Renderer.shaderSource, options: nil)
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "vs_main")
        desc.fragmentFunction = library.makeFunction(name: "fs_main")
        desc.colorAttachments[0].pixelFormat = pixelFormat
        pipeline = try device.makeRenderPipelineState(descriptor: desc)
        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let drawable = view.currentDrawable,
              let passDesc = view.currentRenderPassDescriptor,
              let commandBuffer = queue.makeCommandBuffer() else { return }

        let t = Float(Date().timeIntervalSince(start))
        passDesc.colorAttachments[0].clearColor = MTLClearColor(
            red: Double(0.5 + 0.5 * sin(t * 0.7)),
            green: Double(0.5 + 0.5 * sin(t * 1.3 + 2)),
            blue: Double(0.5 + 0.5 * sin(t * 0.9 + 4)),
            alpha: 1)

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: passDesc) else { return }
        let aspect = Float(view.drawableSize.width / max(view.drawableSize.height, 1))
        var uniforms: [Float] = [t, aspect]
        encoder.setRenderPipelineState(pipeline)
        encoder.setVertexBytes(&uniforms, length: MemoryLayout<Float>.stride * uniforms.count, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()

        frameCount += 1
        onFrame?(frameCount)
    }
}
