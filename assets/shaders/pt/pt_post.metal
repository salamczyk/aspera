#include "pt_common.metal"

kernel void pt_post(constant PtParams& params [[buffer(0)]],
                    texture2d<float, access::read> guideImage [[texture(10)]],
                    texture2d<float, access::read> albedoImage [[texture(11)]],
                    texture2d<float, access::read> historyDiffuse [[texture(13)]],
                    texture2d<float, access::read> historySpecular [[texture(14)]],
                    texture2d<float, access::read> moments [[texture(17)]],
                    texture2d<float, access::read> linearImage [[texture(22)]],
                    texture2d<float, access::write> outputImage [[texture(21)]],
                    uint2 pixel [[thread_position_in_grid]]) {
    uint2 size = params.sizeFrame.xy;
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint2 center = min(pixel, size - 1u);
    uint debug = uint(clamp(params.post.z + 0.5, 0.0, 6.0));
    if (debug == 1u) {
        float3 albedo = clamp(albedoImage.read(center).rgb, float3(0.0), float3(1.0));
        outputImage.write(float4(ptLinearToSrgb(albedo), 1.0), center);
        return;
    }
    if (debug == 2u) {
        float3 normal = clamp(guideImage.read(center).xyz * 0.5 + 0.5, float3(0.0), float3(1.0));
        outputImage.write(float4(normal, 1.0), center);
        return;
    }
    if (debug == 5u) {
        float depth = clamp(guideImage.read(center).w / 64.0, 0.0, 1.0);
        outputImage.write(float4(float3(depth), 1.0), center);
        return;
    }
    if (debug == 6u) {
        float history = clamp(moments.read(center).b / 8.0, 0.0, 1.0);
        outputImage.write(float4(float3(history), 1.0), center);
        return;
    }
    float3 hdr = max(linearImage.read(center).rgb, float3(0.0));
    if (debug == 3u || debug == 4u) {
        hdr = debug == 3u ? max(historyDiffuse.read(center).rgb, float3(0.0))
                          : max(historySpecular.read(center).rgb, float3(0.0));
        float3 mapped = ptAces(hdr * max(params.post.x, 0.0));
        outputImage.write(float4(ptLinearToSrgb(mapped), 1.0), center);
        return;
    }
    float3 bloom = float3(0.0); float bloomWeight = 0.0;
    float bloomStrength = max(params.post.y, 0.0);
    if (bloomStrength > 0.0) {
        float threshold = 1.0 / max(params.post.x, 1.0e-5);
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
            int2 q = clamp(int2(pixel) + int2(x, y), int2(0), int2(size - 1u));
            float3 neighbor = max(linearImage.read(uint2(q)).rgb, float3(0.0));
            float weight = 1.0 / (1.0 + float(x * x + y * y));
            bloom += max(neighbor - float3(threshold), float3(0.0)) * weight; bloomWeight += weight;
        }
        bloom /= max(bloomWeight, 1.0e-5);
    }
    float3 mapped = ptAces((hdr + bloom * bloomStrength) * max(params.post.x, 0.0));
    outputImage.write(float4(ptLinearToSrgb(mapped), 1.0), center);
}
