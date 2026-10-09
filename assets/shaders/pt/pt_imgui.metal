#include <metal_stdlib>
using namespace metal;

struct PtImGuiVertex {
    float2 position [[attribute(0)]];
    float2 texcoord [[attribute(1)]];
    uchar4 color [[attribute(2)]];
};
struct PtImGuiVarying {
    float4 position [[position]];
    float2 texcoord;
    float4 color;
};

vertex PtImGuiVarying pt_imgui_vertex(PtImGuiVertex in [[stage_in]],
                                      constant float4x4& projection [[buffer(2)]]) {
    PtImGuiVarying out;
    out.position = projection * float4(in.position, 0.0, 1.0);
    out.texcoord = in.texcoord;
    out.color = float4(in.color) / 255.0;
    return out;
}

fragment float4 pt_imgui_fragment(PtImGuiVarying in [[stage_in]],
                                  texture2d<float> image [[texture(0)]],
                                  sampler imageSampler [[sampler(0)]]) {
    return image.sample(imageSampler, in.texcoord) * in.color;
}
