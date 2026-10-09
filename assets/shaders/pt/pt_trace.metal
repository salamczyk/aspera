#include "pt_common.metal"

struct PtHit {
    bool hit;
    float distance;
    float3 position;
    float3 previousPosition;
    float3 normal;
    float3 base;
    float roughness;
    float metallic;
    float3 emission;
    uint stableId;
    uint materialId;
};

PtHit ptEmptyHit() {
    PtHit h;
    h.hit = false; h.distance = 0.0; h.position = float3(0.0); h.previousPosition = float3(0.0);
    h.normal = float3(0.0, 1.0, 0.0); h.base = float3(0.0);
    h.roughness = 1.0; h.metallic = 0.0; h.emission = float3(0.0);
    h.stableId = 0u; h.materialId = 0u;
    return h;
}

bool ptTraceClosest(constant PtParams& params,
                    device const PtVertex* vertices,
                    device const uint* indices,
                    device const PtInstance* instances,
                    device const PtMaterial* materials,
                    raytracing::instance_acceleration_structure sceneAS,
                    float3 origin, float3 direction,
                    thread PtHit& hit) {
    hit = ptEmptyHit();
    if (params.options.w == 0u) return false;
    raytracing::ray ray;
    ray.origin = origin; ray.direction = direction;
    ray.min_distance = 0.0005; ray.max_distance = 1.0e30;
    raytracing::intersector<raytracing::triangle_data, raytracing::instancing> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    auto intersection = intersector.intersect(ray, sceneAS, 0xFFu);
    if (intersection.type != raytracing::intersection_type::triangle) return false;
    uint instanceId = intersection.instance_id;
    uint primitiveId = intersection.primitive_id;
    if (instanceId >= params.options.w) return false;
    PtInstance instance = instances[instanceId];
    uint baseIndex = instance.offsets.x + primitiveId * 3u;
    uint i0 = instance.offsets.y + indices[baseIndex + 0u];
    uint i1 = instance.offsets.y + indices[baseIndex + 1u];
    uint i2 = instance.offsets.y + indices[baseIndex + 2u];
    PtVertex v0 = vertices[i0], v1 = vertices[i1], v2 = vertices[i2];
    float2 bary = intersection.triangle_barycentric_coord;
    float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 localPosition = v0.position.xyz * weights.x + v1.position.xyz * weights.y + v2.position.xyz * weights.z;
    float3 localNormal = ptSafeNormalize(v0.normal.xyz * weights.x + v1.normal.xyz * weights.y + v2.normal.xyz * weights.z,
                                          float3(0.0, 1.0, 0.0));
    float4 worldPosition = instance.model * float4(localPosition, 1.0);
    float3x3 inverse3(instance.inverse[0].xyz, instance.inverse[1].xyz, instance.inverse[2].xyz);
    float3 worldNormal = ptSafeNormalize(transpose(inverse3) * localNormal,
                                         float3(0.0, 1.0, 0.0));
    if (dot(worldNormal, direction) > 0.0) worldNormal = -worldNormal;
    uint materialId = uint(max(v0.color.w * weights.x + v1.color.w * weights.y + v2.color.w * weights.z, 0.0) + 0.5);
    PtMaterial material = materials[materialId];
    hit.hit = true;
    hit.distance = intersection.distance;
    hit.position = worldPosition.xyz / max(abs(worldPosition.w), 1.0e-8);
    float4 previousWorldPosition = instance.previous * float4(localPosition, 1.0);
    hit.previousPosition = previousWorldPosition.xyz / max(abs(previousWorldPosition.w), 1.0e-8);
    hit.normal = worldNormal;
    hit.base = clamp(material.baseRoughness.xyz, float3(0.0), float3(1.0));
    hit.roughness = clamp(material.baseRoughness.w, 0.05, 1.0);
    hit.metallic = clamp(material.emissionMetallic.w, 0.0, 1.0);
    hit.emission = max(material.emissionMetallic.xyz, float3(0.0));
    hit.stableId = instance.offsets.z;
    hit.materialId = materialId;
    return true;
}

bool ptOccluded(const constant PtParams& params, raytracing::instance_acceleration_structure sceneAS,
                float3 origin, float3 direction, float distance) {
    if (params.options.w == 0u || distance <= 0.0005) return false;
    raytracing::ray ray;
    ray.origin = origin; ray.direction = direction;
    ray.min_distance = 0.0005; ray.max_distance = max(distance - 0.001, 0.0006);
    raytracing::intersector<raytracing::instancing> intersector;
    intersector.assume_geometry_type(raytracing::geometry_type::triangle);
    auto intersection = intersector.intersect(ray, sceneAS, 0xFFu);
    return intersection.type == raytracing::intersection_type::triangle;
}

void ptAddSun(constant PtParams& params, raytracing::instance_acceleration_structure sceneAS, PtHit h,
              float3 view, float3 throughput, bool continuation, thread float3& diffuseOut,
              thread float3& specularOut, thread uint& rng) {
    float3 sun = ptSunDirection(params);
    float c = ptSunCosine(params);
    float u0 = ptRandom(rng), u1 = ptRandom(rng);
    float cosTheta = mix(1.0, c, u0), sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 l = ptToWorld(float3(sinTheta * cos(PT_TWO_PI * u1), sinTheta * sin(PT_TWO_PI * u1), cosTheta), sun);
    float noL = max(dot(h.normal, l), 0.0);
    if (noL <= 0.0 || ptOccluded(params, sceneAS, h.position + h.normal * 0.001, l, 1.0e30)) return;
    float3 fd, fs; float pd, ps, mixture;
    ptEvalBsdf(h.base, h.metallic, h.roughness, h.normal, view, l, fd, fs, pd, ps, mixture);
    float lightPdf = ptSunPdf(l, params), weight = continuation ? ptPowerHeuristic(lightPdf, mixture) : 1.0;
    float3 li = ptSunRadiance(params);
    diffuseOut += throughput * fd * li * noL * weight / max(lightPdf, 1.0e-8);
    specularOut += throughput * fs * li * noL * weight / max(lightPdf, 1.0e-8);
}

void ptAddSky(constant PtParams& params, raytracing::instance_acceleration_structure sceneAS, PtHit h,
              float3 view, float3 throughput, bool continuation, thread float3& diffuseOut,
              thread float3& specularOut, thread uint& rng) {
    float lightPdf; float3 l = ptSampleCosine(h.normal, rng, lightPdf);
    if (ptInSunDisk(l, params)) return;
    if (ptOccluded(params, sceneAS, h.position + h.normal * 0.001, l, 1.0e30)) return;
    float3 fd, fs; float pd, ps, mixture;
    ptEvalBsdf(h.base, h.metallic, h.roughness, h.normal, view, l, fd, fs, pd, ps, mixture);
    float weight = continuation ? ptPowerHeuristic(lightPdf, mixture) : 1.0;
    float noL = max(dot(h.normal, l), 0.0);
    float3 li = ptSkyOnly(l, params);
    diffuseOut += throughput * fd * li * noL * weight / max(lightPdf, 1.0e-8);
    specularOut += throughput * fs * li * noL * weight / max(lightPdf, 1.0e-8);
}

void ptAddPointLight(constant PtParams& params, raytracing::instance_acceleration_structure sceneAS,
                     device const PtLight* lights, PtHit h, float3 view, float3 throughput,
                     thread float3& diffuseOut, thread float3& specularOut, thread uint& rng) {
    uint count = params.counts.z;
    if (count == 0u) return;
    uint lightId = min(uint(ptRandom(rng) * float(count)), count - 1u);
    PtLight light = lights[lightId];
    float3 delta = light.positionRange.xyz - h.position;
    float distance = length(delta);
    if (distance <= 0.001 || distance > max(light.positionRange.w, 0.001)) return;
    float3 l = delta / distance; float noL = max(dot(h.normal, l), 0.0);
    if (noL <= 0.0) return;
    float spot = 1.0;
    if (light.directionType.w > 0.5) {
        float cc = dot(-l, ptSafeNormalize(light.directionType.xyz, float3(0.0, -1.0, 0.0)));
        spot = light.cone.x - light.cone.y > 1.0e-5
            ? smoothstep(light.cone.y, light.cone.x, cc) : step(light.cone.y, cc);
        if (spot <= 0.0) return;
    }
    if (light.cone.z > 0.5 && ptOccluded(params, sceneAS, h.position + h.normal * 0.001, l, distance)) return;
    float3 fd, fs; float pd, ps, mixture;
    ptEvalBsdf(h.base, h.metallic, h.roughness, h.normal, view, l, fd, fs, pd, ps, mixture);
    // colorIntensity.w is already normalized by the authored source's
    // spherical solid angle on the CPU.  Apply the same finite-range fade as
    // the raster backend without dividing by 4*pi a second time.
    float edge = clamp(1.0 - pow(distance / max(light.positionRange.w, 0.001), 4.0), 0.0, 1.0);
    float attenuation = edge * edge / max(distance * distance, 0.01);
    float3 li = max(light.colorIntensity.xyz, float3(0.0)) * max(light.colorIntensity.w, 0.0) *
                spot * attenuation;
    float scale = float(count);
    diffuseOut += throughput * fd * li * noL * scale;
    specularOut += throughput * fs * li * noL * scale;
}

bool ptChooseEmitter(const constant PtParams& params, device const PtEmitter* emitters,
                     thread uint& rng, thread PtEmitter& emitter, thread float& selectionPdf) {
    uint count = params.counts.w;
    if (count == 0u) return false;
    float pick = ptRandom(rng), previous = 0.0;
    uint chosen = count - 1u;
    for (uint i = 0u; i < count; ++i) {
        float cdf = emitters[i].bCdf.w;
        if (pick <= cdf) {
            chosen = i; selectionPdf = emitters[i].radiance.w > 0.0 ? emitters[i].radiance.w : max(cdf - previous, 1.0e-8);
            emitter = emitters[i]; return true;
        }
        previous = cdf;
    }
    emitter = emitters[chosen];
    selectionPdf = emitter.radiance.w > 0.0 ? emitter.radiance.w : max(emitter.bCdf.w - previous, 1.0e-8);
    return true;
}

void ptAddEmitter(constant PtParams& params, raytracing::instance_acceleration_structure sceneAS,
                  device const PtEmitter* emitters, PtHit h, float3 view, float3 throughput,
                  bool continuation,
                  thread float3& diffuseOut, thread float3& specularOut, thread uint& rng) {
    PtEmitter emitter; float selectionPdf;
    if (!ptChooseEmitter(params, emitters, rng, emitter, selectionPdf)) return;
    float su = sqrt(ptRandom(rng)), sv = ptRandom(rng);
    float3 a = emitter.aArea.xyz, b = emitter.bCdf.xyz, c = emitter.cPad.xyz;
    float3 lightPosition = a * (1.0 - su) + b * (su * (1.0 - sv)) + c * (su * sv);
    float3 lightNormal = ptSafeNormalize(cross(b - a, c - a), float3(0.0, 1.0, 0.0));
    float3 delta = lightPosition - h.position; float distance2 = dot(delta, delta);
    if (distance2 <= 1.0e-6) return;
    float distance = sqrt(distance2); float3 l = delta / distance;
    float noL = max(dot(h.normal, l), 0.0), lightNoL = max(dot(lightNormal, -l), 0.0);
    float area = max(emitter.aArea.w, 1.0e-8);
    if (noL <= 0.0 || lightNoL <= 0.0 || ptOccluded(params, sceneAS, h.position + h.normal * 0.001, l, distance)) return;
    float pdf = selectionPdf * distance2 / max(area * lightNoL, 1.0e-8);
    float3 fd, fs; float pd, ps, mixture;
    ptEvalBsdf(h.base, h.metallic, h.roughness, h.normal, view, l, fd, fs, pd, ps, mixture);
    float weight = continuation ? ptPowerHeuristic(pdf, mixture) : 1.0;
    float3 li = max(emitter.radiance.xyz, float3(0.0));
    diffuseOut += throughput * fd * li * noL * weight / max(pdf, 1.0e-8);
    specularOut += throughput * fs * li * noL * weight / max(pdf, 1.0e-8);
}

float ptEmitterPdfAtPoint(constant PtParams& params, device const PtEmitter* emitters,
                          float3 origin, float3 point, float3 direction) {
    uint count = params.counts.w;
    float distance2 = dot(point - origin, point - origin);
    if (distance2 <= 1.0e-8) return 0.0;
    for (uint i = 0u; i < count; ++i) {
        PtEmitter emitter = emitters[i];
        float3 a = emitter.aArea.xyz, b = emitter.bCdf.xyz, c = emitter.cPad.xyz;
        float3 ab = b - a, ac = c - a, ap = point - a;
        float d00 = dot(ab, ab), d01 = dot(ab, ac), d11 = dot(ac, ac);
        float d20 = dot(ap, ab), d21 = dot(ap, ac);
        float denom = d00 * d11 - d01 * d01;
        if (abs(denom) < 1.0e-8) continue;
        float v = (d11 * d20 - d01 * d21) / denom;
        float w = (d00 * d21 - d01 * d20) / denom;
        float3 closest = a + ab * v + ac * w;
        if (v < -0.002 || w < -0.002 || v + w > 1.002 || dot(closest - point, closest - point) > 0.0001) continue;
        float3 normal = ptSafeNormalize(cross(ab, ac), float3(0.0, 1.0, 0.0));
        float lightCos = abs(dot(normal, -direction));
        if (lightCos <= 1.0e-6) return 0.0;
        return max(emitter.radiance.w, 0.0) * distance2 / max(emitter.aArea.w * lightCos, 1.0e-8);
    }
    return 0.0;
}

void ptAddEnvironment(const constant PtParams& params, float3 direction, float3 normal,
                      float3 throughput, float previousPdf, bool lastSpecular,
                      thread float3& diffuseOut, thread float3& specularOut) {
    bool sun = ptInSunDisk(direction, params);
    float3 li = sun ? ptSunRadiance(params) : ptSkyOnly(direction, params);
    float environmentPdf = sun ? ptSunPdf(direction, params) : ptSkyPdf(direction, normal, params);
    float weight = previousPdf > 0.0 ? ptPowerHeuristic(previousPdf, environmentPdf) : 1.0;
    float3 value = throughput * li * weight;
    if (lastSpecular) specularOut += value; else diffuseOut += value;
}

kernel void pt_trace(constant PtParams& params [[buffer(0)]],
                     device const PtVertex* vertices [[buffer(1)]],
                     device const uint* indices [[buffer(2)]],
                     device const PtInstance* instances [[buffer(3)]],
                     device const PtMaterial* materials [[buffer(4)]],
                     device const PtLight* lights [[buffer(5)]],
                     device const PtEmitter* emitters [[buffer(6)]],
                    raytracing::instance_acceleration_structure sceneAS [[buffer(7)]],
                     texture2d<float, access::write> rawDiffuse [[texture(8)]],
                     texture2d<float, access::write> rawSpecular [[texture(9)]],
                     texture2d<float, access::write> guideImage [[texture(10)]],
                     texture2d<float, access::write> albedoImage [[texture(11)]],
                     texture2d<float, access::write> motionImage [[texture(12)]],
                     uint2 pixel [[thread_position_in_grid]]) {
    uint2 size = params.sizeFrame.xy;
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint sampleCount = min(params.counts.x, 64u);
    if (sampleCount == 0u) {
        rawDiffuse.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
        rawSpecular.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
        guideImage.write(float4(0.0), pixel);
        albedoImage.write(float4(0.0, 0.0, 0.0, 1.0), pixel);
        motionImage.write(float4(-1.0), pixel);
        return;
    }
    float3 diffuseSum = float3(0.0), specularSum = float3(0.0);
    PtHit primary = ptEmptyHit();
    float3 primaryDirection = float3(0.0, 0.0, -1.0);
    uint bounceLimit = min(max(params.counts.y, 1u), 12u);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex) {
        uint globalSampleIndex = params.sizeFrame.z + sampleIndex;
        // The sample stream is a function only of pixel and global sample;
        // history resets must not change the Monte Carlo sequence.
        uint seed = ptHash(pixel.x + pixel.y * 65537u + globalSampleIndex * 747796405u);
        float2 uv = (float2(pixel) + 0.5) / float2(size);
        uv += (float2(ptRandom(seed), ptRandom(seed)) - 0.5) / float2(size);
        float3 nearPoint = ptUnproject(uv, 0.0, params.inverseVP);
        float3 farPoint = ptUnproject(uv, 1.0, params.inverseVP);
        float3 direction = ptSafeNormalize(farPoint - nearPoint, float3(0.0, 0.0, -1.0));
        if (sampleIndex == 0u) primaryDirection = direction;
        float3 origin = params.eye.xyz, diffuseOut = float3(0.0), specularOut = float3(0.0);
        float3 throughput = float3(1.0), previousNormal = -direction;
        float previousPdf = 0.0; bool lastSpecular = false;
        for (uint bounce = 0u; bounce < 12u && bounce < bounceLimit; ++bounce) {
            PtHit hit;
            if (!ptTraceClosest(params, vertices, indices, instances, materials, sceneAS, origin, direction, hit)) {
                ptAddEnvironment(params, direction, previousNormal, throughput, previousPdf, lastSpecular, diffuseOut, specularOut);
                break;
            }
            if (sampleIndex == 0u && bounce == 0u) primary = hit;
            if (ptMax3(hit.emission) > 0.0) {
                float emitterPdf = ptEmitterPdfAtPoint(params, emitters, origin, hit.position, direction);
                float emissionWeight = previousPdf > 0.0 && emitterPdf > 0.0 ? ptPowerHeuristic(previousPdf, emitterPdf) : 1.0;
                float3 emitted = throughput * hit.emission * emissionWeight;
                if (lastSpecular) specularOut += emitted; else diffuseOut += emitted;
            }
            float3 view = ptSafeNormalize(-direction, hit.normal);
            bool continuation = bounce + 1u < bounceLimit;
            ptAddSun(params, sceneAS, hit, view, throughput, continuation, diffuseOut, specularOut, seed);
            ptAddPointLight(params, sceneAS, lights, hit, view, throughput, diffuseOut, specularOut, seed);
            ptAddSky(params, sceneAS, hit, view, throughput, continuation, diffuseOut, specularOut, seed);
            ptAddEmitter(params, sceneAS, emitters, hit, view, throughput, continuation, diffuseOut, specularOut, seed);
            if (!continuation) break;
            float3 nextDirection, bsdfValue; float bsdfPdf; bool sampledSpecular;
            if (!ptSampleBsdf(hit.base, hit.metallic, hit.roughness, hit.normal, view, seed,
                              nextDirection, bsdfValue, bsdfPdf, sampledSpecular)) break;
            float noL = max(dot(hit.normal, nextDirection), 0.0);
            throughput *= bsdfValue * noL / max(bsdfPdf, 1.0e-8);
            if (bounce >= 2u) {
                float survival = clamp(ptMax3(throughput), 0.05, 0.95);
                if (ptRandom(seed) > survival) break;
                throughput /= survival;
            }
            previousNormal = hit.normal; previousPdf = bsdfPdf; lastSpecular = sampledSpecular;
            origin = hit.position + hit.normal * (dot(hit.normal, nextDirection) >= 0.0 ? 0.001 : -0.001);
            direction = nextDirection;
        }
        diffuseSum += diffuseOut;
        specularSum += specularOut;
    }
    float sampleScale = 1.0 / float(sampleCount);
    float3 diffuseOut = diffuseSum * sampleScale;
    float3 specularOut = specularSum * sampleScale;
    float4 guide = float4(0.0), albedo = float4(0.0, 0.0, 0.0, 1.0), motion = float4(-1.0);
    if (primary.hit) {
        guide = float4(primary.normal, primary.distance); albedo = float4(primary.base, primary.roughness);
        float4 previousClip = params.previousVP * float4(primary.previousPosition, 1.0);
        if (previousClip.w > 1.0e-8) {
            float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
            previousUv.y = 1.0 - previousUv.y;
            motion = float4(previousUv, float(primary.stableId), float(primary.materialId));
        }
    } else {
        // Preserve environment history while the camera rotates.
        float4 previousClip = params.previousVP * float4(primaryDirection, 0.0);
        if (previousClip.w > 1.0e-8) {
            float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
            previousUv.y = 1.0 - previousUv.y;
            motion = float4(previousUv, 0.0, 0.0);
        }
    }
    rawDiffuse.write(float4(max(diffuseOut, float3(0.0)), 1.0), pixel);
    rawSpecular.write(float4(max(specularOut, float3(0.0)), 1.0), pixel);
    guideImage.write(guide, pixel); albedoImage.write(albedo, pixel); motionImage.write(motion, pixel);
}
