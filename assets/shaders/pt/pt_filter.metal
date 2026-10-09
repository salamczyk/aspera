#include "pt_common.metal"

float4 ptFilterLoad(uint pass, uint2 p,
                    texture2d<float, access::read> historyDiffuse,
                    texture2d<float, access::read> historySpecular,
                    texture2d<float, access::read_write> filterA,
                    texture2d<float, access::read_write> filterB) {
    if (pass == 0u) return historyDiffuse.read(p) + historySpecular.read(p);
    return (pass & 1u) != 0u ? filterA.read(p) : filterB.read(p);
}

void ptFilterStore(uint pass, uint2 p, float4 value,
                   texture2d<float, access::read_write> filterA,
                   texture2d<float, access::read_write> filterB,
                   texture2d<float, access::read_write> scratchA) {
    if (pass == 0u || pass == 2u) filterA.write(value, p);
    else if (pass == 1u || pass == 3u) filterB.write(value, p);
    else scratchA.write(value, p);
}
float ptFilterKernel(int i) { return i == 0 ? 0.5 : 0.25; }

// Guides already contain unit normals. Integer power avoids a general pow/log.
float ptNormalWeight(float3 a, float3 b) {
    float w = clamp(dot(a,b),0.0,1.0);
    w *= w; w *= w; w *= w; w *= w; return w*w;
}

kernel void pt_filter(constant PtParams& params [[buffer(0)]], constant uint& pass [[buffer(8)]],
                      texture2d<float, access::read> historyDiffuse [[texture(13)]],
                      texture2d<float, access::read> historySpecular [[texture(14)]],
                      texture2d<float, access::read> historyGuide [[texture(15)]],
                      texture2d<float, access::read> moments [[texture(17)]],
                      texture2d<float, access::read_write> filterA [[texture(19)]],
                      texture2d<float, access::read_write> filterB [[texture(20)]],
                      texture2d<float, access::read_write> scratchA [[texture(22)]],
                      uint2 pixel [[thread_position_in_grid]]) {
    uint2 size = params.sizeFrame.xy;
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint2 centerPixel = min(pixel, size - 1u);
    float4 center = ptFilterLoad(pass, centerPixel, historyDiffuse, historySpecular, filterA, filterB);
    float4 centerGuide = historyGuide.read(centerPixel);
    float2 centerMoments = moments.read(centerPixel).xy;
    float variance = max(centerMoments.y - centerMoments.x * centerMoments.x, 0.0);
    if (params.options.x == 0u || pass >= 5u) {
        ptFilterStore(pass, centerPixel, float4(max(center.rgb, float3(0.0)), 1.0), filterA, filterB, scratchA);
        return;
    }
    float sigma = sqrt(variance + 1.0e-5), centerLum = ptLuminance(center.rgb);
    float minLum = max(0.0, centerMoments.x - 2.5 * sigma), maxLum = centerMoments.x + 2.5 * sigma;
    if (centerLum > 1.0e-5) center.rgb *= clamp(centerLum, minLum, maxLum) / centerLum;
    float3 result = float3(0.0); float totalWeight = 0.0;
    int stepSize = 1 << int(pass);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 sampleInt = int2(pixel) + int2(x, y) * stepSize;
        uint2 samplePixel = uint2(clamp(sampleInt, int2(0), int2(size - 1u)));
        float4 sampleValue = ptFilterLoad(pass, samplePixel, historyDiffuse, historySpecular, filterA, filterB);
        float4 sampleGuide = historyGuide.read(samplePixel);
        float spatial = ptFilterKernel(x) * ptFilterKernel(y);
        bool centerSurface = dot(centerGuide.xyz, centerGuide.xyz) > 0.01;
        bool sampleSurface = dot(sampleGuide.xyz, sampleGuide.xyz) > 0.01;
        float normalWeight = !centerSurface && !sampleSurface ? 1.0 :
                             centerSurface && sampleSurface ? ptNormalWeight(centerGuide.xyz, sampleGuide.xyz) : 0.0;
        float depthWeight = exp(-abs(centerGuide.w - sampleGuide.w) / max(abs(centerGuide.w), 0.05) * 4.0);
        float varianceWeight = exp(-abs(ptLuminance(sampleValue.rgb) - centerMoments.x) / (sigma + 0.01));
        float weight = spatial * normalWeight * depthWeight * varianceWeight;
        result += sampleValue.rgb * weight; totalWeight += weight;
    }
    result = totalWeight > 1.0e-6 ? result / totalWeight : center.rgb;
    ptFilterStore(pass, centerPixel, float4(max(result, float3(0.0)), 1.0), filterA, filterB, scratchA);
}
