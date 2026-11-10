// x86Sim: stage-4 compute kernels for mtlprobe (compiled on the Mac for the iOS simulator:
// tests/metal/shaders/build.sh -> probe.metallib; mtlprobe also compiles this source at run time).
#include <metal_stdlib>
using namespace metal;

kernel void vadd(device const float* a [[buffer(0)]], device const float* b [[buffer(1)]],
                 device float* c [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    c[i] = a[i] + b[i];
}

struct Params { float scale; float bias; uint count; };

// setBytes: params come from a small constant buffer
kernel void saxpy_bytes(device const float* x [[buffer(0)]], device float* y [[buffer(1)]],
                        constant Params& p [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    if (i < p.count) y[i] = p.scale * x[i] + p.bias;
}

// texture write: RGBA8 colour from the coordinates
kernel void fill_tex(texture2d<float, access::write> t [[texture(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= t.get_width() || g.y >= t.get_height()) return;
    t.write(float4(float(g.x) / 255.0, float(g.y) / 255.0, float((g.x + g.y) & 255) / 255.0, 1.0), g);
}

// threadgroup memory reduction: one partial sum per threadgroup (threadgroup size 256)
kernel void reduce_sum(device const uint* in [[buffer(0)]], device uint* partial [[buffer(1)]],
                       threadgroup uint* scratch [[threadgroup(0)]],
                       uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]],
                       uint tg [[threadgroup_position_in_grid]], uint tsize [[threads_per_threadgroup]]) {
    scratch[lid] = in[gid];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = tsize / 2; s > 0; s >>= 1) {
        if (lid < s) scratch[lid] += scratch[lid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0) partial[tg] = scratch[0];
}

// function constants
constant float fc_scale [[function_constant(0)]];
constant bool fc_negate [[function_constant(1)]];
constant int fc_offset [[function_constant(2)]];
kernel void fconst(device const float* x [[buffer(0)]], device float* y [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    float v = x[i] * fc_scale + float(fc_offset);
    y[i] = fc_negate ? -v : v;
}

// sampler + texture read: copies a texture into a buffer (setSamplerState)
kernel void sample_tex(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device float4* out [[buffer(0)]],
                       uint2 g [[thread_position_in_grid]]) {
    float2 uv = (float2(g) + 0.5) / float2(t.get_width(), t.get_height());
    out[g.y * t.get_width() + g.x] = t.sample(s, uv, level(0));
}

// pointers in memory (gpuAddress): the buffer holds addresses of other buffers
struct Ptrs { device const float* src; device float* dst; uint count; };
kernel void via_ptr(constant Ptrs& p [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i < p.count) p.dst[i] = p.src[i] * 2.0 + 1.0;
}

// ---- stage 5: render pipelines (mtlprobe render tests)
struct VOut { float4 pos [[position]]; float4 color; float2 uv; };

// positions (xyz) and colours from buffers by vertex id: no vertex descriptor
vertex VOut tri_vs(uint vid [[vertex_id]], constant float4* pos [[buffer(0)]], constant float4* col [[buffer(1)]]) {
    VOut o;
    o.pos = float4(pos[vid].xyz, 1.0);
    o.color = col[vid];
    o.uv = float2(0.0);
    return o;
}
fragment float4 color_fs(VOut in [[stage_in]]) { return in.color; }

// vertex descriptor (stage_in), a transform from setVertexBytes
struct QIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct Xf { float2 scale; float2 offset; };
vertex VOut quad_vs(QIn in [[stage_in]], constant Xf& xf [[buffer(1)]]) {
    VOut o;
    o.pos = float4(in.pos * xf.scale + xf.offset, 0.0, 1.0);
    o.color = float4(1.0);
    o.uv = in.uv;
    return o;
}
fragment float4 tex_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                       constant float4& tint [[buffer(0)]]) {
    return t.sample(s, in.uv) * tint;
}

// solid colour from setFragmentBytes (blending, depth tests)
fragment float4 solid_fs(VOut in [[stage_in]], constant float4& c [[buffer(0)]]) { return c; }
