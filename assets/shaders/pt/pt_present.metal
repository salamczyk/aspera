#include <metal_stdlib>
using namespace metal;

struct PtPresentVarying {
    float4 position [[position]];
    float2 texcoord;
};

vertex PtPresentVarying pt_present_vertex(uint vertexId [[vertex_id]]) {
    constexpr float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    float2 p = positions[vertexId];
    PtPresentVarying out;
    out.position = float4(p, 0.0, 1.0);
    out.texcoord = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

fragment float4 pt_present_fragment(PtPresentVarying in [[stage_in]],
                                    texture2d<float> outputTexture [[texture(0)]],
                                    sampler outputSampler [[sampler(0)]]) {
    return outputTexture.sample(outputSampler, in.texcoord);
}
