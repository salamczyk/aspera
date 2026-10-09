#include "pt_common.metal"

float4 ptTemporalLoad(texture2d<float, access::read> image, float2 uv, uint2 size) {
    float2 q = clamp(uv, 0.0, 1.0) * float2(size);
    uint2 p = uint2(clamp(q, 0.0, float2(size - 1u)));
    return image.read(p);
}

void ptRawNeighborhood(texture2d<float, access::read> rawDiffuse,
                       texture2d<float, access::read> rawSpecular,
                       uint2 center, uint2 size,
                       thread float3& minDiffuse, thread float3& maxDiffuse,
                       thread float3& minSpecular, thread float3& maxSpecular) {
    minDiffuse = minSpecular = float3(1.0e30);
    maxDiffuse = maxSpecular = float3(-1.0e30);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 q = clamp(int2(center) + int2(x, y), int2(0), int2(size - 1u));
        float3 d = rawDiffuse.read(uint2(q)).rgb;
        float3 s = rawSpecular.read(uint2(q)).rgb;
        minDiffuse = min(minDiffuse, d); maxDiffuse = max(maxDiffuse, d);
        minSpecular = min(minSpecular, s); maxSpecular = max(maxSpecular, s);
    }
}

kernel void pt_temporal(constant PtParams& params [[buffer(0)]],
                        texture2d<float, access::read> rawDiffuse [[texture(8)]],
                        texture2d<float, access::read> rawSpecular [[texture(9)]],
                        texture2d<float, access::read> guideImage [[texture(10)]],
                        texture2d<float, access::read> albedoImage [[texture(11)]],
                        texture2d<float, access::read> motionImage [[texture(12)]],
                        texture2d<float, access::read_write> historyDiffuse [[texture(13)]],
                        texture2d<float, access::read_write> historySpecular [[texture(14)]],
                        texture2d<float, access::read_write> historyGuide [[texture(15)]],
                        texture2d<float, access::read_write> historyMotion [[texture(16)]],
                        texture2d<float, access::read_write> moments [[texture(17)]],
                        texture2d<float, access::read> previousMoments [[texture(18)]],
                        texture2d<float, access::read> previousGuide [[texture(24)]],
                        texture2d<float, access::read> previousMotion [[texture(25)]],
                        texture2d<float, access::read> previousDiffuse [[texture(26)]],
                        texture2d<float, access::read> previousSpecular [[texture(27)]],
                        uint2 pixel [[thread_position_in_grid]]) {
    uint2 size = params.sizeFrame.xy;
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    float4 currentDiffuse = rawDiffuse.read(pixel);
    float4 currentSpecular = rawSpecular.read(pixel);
    float4 currentGuide = guideImage.read(pixel);
    float4 currentMotion = motionImage.read(pixel);
    bool reset = params.sizeFrame.w != 0u;
    bool currentSurface = dot(currentGuide.xyz, currentGuide.xyz) > 0.01;
    float2 previousUv = currentMotion.xy;
    bool validUv = all(previousUv >= 0.0) && all(previousUv <= 1.0);
    float4 oldDiffuse = float4(0.0), oldSpecular = float4(0.0);
    float4 oldGuide = float4(0.0), oldMotion = float4(0.0), oldMoment = float4(0.0);
    float historyWeight = 0.0;
    bool stationaryReuse = !reset && params.options.y != 0u;
    if (stationaryReuse) {
        // Keep stationary reference accumulation at the same output pixel so
        // camera jitter crossing a silhouette remains unbiased.
        oldDiffuse = previousDiffuse.read(pixel);
        oldSpecular = previousSpecular.read(pixel);
        oldGuide = previousGuide.read(pixel);
        oldMotion = previousMotion.read(pixel);
        oldMoment = previousMoments.read(pixel);
        historyWeight = max(float(params.options.z), 0.0);
    } else if (!reset && validUv) {
        oldDiffuse = ptTemporalLoad(previousDiffuse, previousUv, size);
        oldSpecular = ptTemporalLoad(previousSpecular, previousUv, size);
        oldGuide = ptTemporalLoad(previousGuide, previousUv, size);
        oldMotion = ptTemporalLoad(previousMotion, previousUv, size);
        oldMoment = ptTemporalLoad(previousMoments, previousUv, size);
        bool oldSurface = dot(oldGuide.xyz, oldGuide.xyz) > 0.01;
        float4 reprojectedGuide = oldGuide;
        float4 reprojectedMotion = oldMotion;
        bool skyCompatible = !currentSurface && !oldSurface;
        bool guideCompatible = skyCompatible ||
            (dot(normalize(currentGuide.xyz), normalize(reprojectedGuide.xyz)) > 0.90 &&
             abs(currentGuide.w - reprojectedGuide.w) <= max(0.05, currentGuide.w * 0.04));
        bool identityCompatible = !currentSurface ||
            (abs(currentMotion.z - reprojectedMotion.z) < 0.5 &&
             abs(currentMotion.w - reprojectedMotion.w) < 0.5);
        if (guideCompatible && identityCompatible) {
            float frameSampleCount = max(float(params.counts.x), 1.0);
            historyWeight = min(max(oldMoment.z, 0.0), 7.0 * frameSampleCount);
            if (historyWeight > 0.0) {
                float3 minDiffuse, maxDiffuse, minSpecular, maxSpecular;
                ptRawNeighborhood(rawDiffuse, rawSpecular, pixel, size,
                                  minDiffuse, maxDiffuse, minSpecular, maxSpecular);
                float variance = max(oldMoment.y - oldMoment.x * oldMoment.x, 0.0);
                float expand = 1.5 * sqrt(variance) + 0.01;
                oldDiffuse.rgb = clamp(oldDiffuse.rgb, minDiffuse - expand, maxDiffuse + expand);
                oldSpecular.rgb = clamp(oldSpecular.rgb, minSpecular - expand, maxSpecular + expand);
            }
        }
    }
    float sampleCount = max(float(params.counts.x), 1.0);
    float totalWeight = historyWeight + sampleCount;
    float3 resolvedDiffuse = (historyWeight > 0.0 ? oldDiffuse.rgb * historyWeight : float3(0.0)) + currentDiffuse.rgb * sampleCount;
    float3 resolvedSpecular = (historyWeight > 0.0 ? oldSpecular.rgb * historyWeight : float3(0.0)) + currentSpecular.rgb * sampleCount;
    resolvedDiffuse /= max(totalWeight, 1.0e-5); resolvedSpecular /= max(totalWeight, 1.0e-5);
    float4 resolvedGuide = currentGuide, resolvedMotion = currentMotion;
    if (!stationaryReuse && historyWeight > 0.0 && !currentSurface && dot(oldGuide.xyz, oldGuide.xyz) > 0.01) { resolvedGuide = oldGuide; resolvedMotion = oldMotion; }
    float luminance = ptLuminance(currentDiffuse.rgb + currentSpecular.rgb), second = luminance * luminance;
    float2 resolvedMoments = historyWeight > 0.0
        ? (oldMoment.xy * historyWeight + float2(luminance, second) * sampleCount) / max(totalWeight, 1.0e-5)
        : float2(luminance, second);
    if (reset) resolvedMoments = float2(luminance, second);
    historyDiffuse.write(float4(resolvedDiffuse, 1.0), pixel);
    historySpecular.write(float4(resolvedSpecular, 1.0), pixel);
    historyGuide.write(resolvedGuide, pixel); historyMotion.write(resolvedMotion, pixel);
    moments.write(float4(resolvedMoments, min(totalWeight, 65535.0), 1.0), pixel);
}
