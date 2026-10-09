#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;

constant float PT_PI = 3.14159265358979323846;
constant float PT_TWO_PI = 6.28318530717958647692;

struct PtVertex { float4 position; float4 normal; float4 color; };
struct PtMaterial { float4 baseRoughness; float4 emissionMetallic; };
struct PtInstance { float4x4 model; float4x4 inverse; float4x4 previous; uint4 offsets; };
struct PtLight { float4 positionRange; float4 colorIntensity; float4 directionType; float4 cone; };
struct PtEmitter { float4 aArea; float4 bCdf; float4 cPad; float4 radiance; };
struct PtParams {
    float4x4 inverseVP;
    float4x4 viewProjection;
    float4x4 previousVP;
    float4 eye;
    float4 sunDirectionIntensity;
    float4 sunColorRadius;
    float4 skyColorIntensity;
    float4 post;
    uint4 sizeFrame;
    uint4 counts;
    uint4 options;
};

uint ptHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float ptRandom(thread uint& state) {
    state = ptHash(state + 0x9e3779b9u);
    return (float(state) + 0.5) / 4294967296.0;
}
float ptSaturate(float x) { return clamp(x, 0.0, 1.0); }
float3 ptSafeNormalize(float3 v, float3 fallback) {
    float l2 = dot(v, v);
    return l2 > 1.0e-12 ? v * rsqrt(l2) : fallback;
}
void ptBasis(float3 n, thread float3& t, thread float3& b) {
    float3 a = fabs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    t = ptSafeNormalize(cross(a, n), float3(1.0, 0.0, 0.0));
    b = cross(n, t);
}
float3 ptToWorld(float3 v, float3 n) {
    float3 t, b; ptBasis(n, t, b);
    return ptSafeNormalize(t * v.x + b * v.y + n * v.z, n);
}
float3 ptSampleCosine(float3 n, thread uint& rng, thread float& pdf) {
    float u0 = ptRandom(rng), u1 = ptRandom(rng);
    float r = sqrt(u0), phi = PT_TWO_PI * u1;
    float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u0)));
    pdf = local.z / PT_PI;
    return ptToWorld(local, n);
}
float ptMax3(float3 v) { return max(v.x, max(v.y, v.z)); }
float ptPowerHeuristic(float a, float b) {
    float aa = a * a, bb = b * b;
    return aa / max(aa + bb, 1.0e-8);
}
float ptDggx(float noH, float alpha) {
    float a2 = alpha * alpha;
    float d = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / max(PT_PI * d * d, 1.0e-8);
}
float ptSmithG1(float noV, float alpha) {
    float a2 = alpha * alpha, noV2 = noV * noV;
    return 2.0 * noV / max(noV + sqrt(a2 + (1.0 - a2) * noV2), 1.0e-6);
}
float3 ptFresnel(float3 f0, float voH) {
    return f0 + (float3(1.0) - f0) * pow(1.0 - ptSaturate(voH), 5.0);
}
float ptSpecularProbability(float3 base, float metallic) {
    float3 f0 = mix(float3(0.04), base, metallic);
    return clamp(0.25 + 0.5 * metallic + 0.25 * ptMax3(f0), 0.05, 0.95);
}
void ptEvalBsdf(float3 base, float metallic, float roughness, float3 n, float3 v, float3 l,
                thread float3& diffuse, thread float3& specular,
                thread float& pdfDiffuse, thread float& pdfSpecular, thread float& mixturePdf) {
    float noV = max(dot(n, v), 0.0), noL = max(dot(n, l), 0.0);
    float3 h = ptSafeNormalize(v + l, n);
    float noH = max(dot(n, h), 0.0), voH = max(dot(v, h), 0.0);
    float alpha = max(roughness * roughness, 0.0025);
    float d = ptDggx(noH, alpha), g = ptSmithG1(noV, alpha) * ptSmithG1(noL, alpha);
    float3 f = ptFresnel(mix(float3(0.04), base, metallic), voH);
    // Preserve the dielectric energy removed by the Fresnel term from the
    // diffuse lobe.  Metallic surfaces already have no diffuse contribution.
    diffuse = (float3(1.0) - f) * (1.0 - metallic) * base / PT_PI;
    specular = d * g * f / max(4.0 * noV * noL, 1.0e-5);
    pdfDiffuse = noL / PT_PI;
    pdfSpecular = d * noH / max(4.0 * voH, 1.0e-5);
    float ps = ptSpecularProbability(base, metallic);
    mixturePdf = (1.0 - ps) * pdfDiffuse + ps * pdfSpecular;
}
bool ptSampleBsdf(float3 base, float metallic, float roughness, float3 n, float3 v,
                  thread uint& rng, thread float3& l, thread float3& value,
                  thread float& pdf, thread bool& sampledSpecular) {
    float ps = ptSpecularProbability(base, metallic);
    sampledSpecular = ptRandom(rng) < ps;
    float alpha = max(roughness * roughness, 0.0025);
    if (sampledSpecular) {
        float u0 = ptRandom(rng), u1 = ptRandom(rng), phi = PT_TWO_PI * u1;
        float a2 = alpha * alpha;
        float cosTheta = sqrt((1.0 - u0) / max(1.0 + (a2 - 1.0) * u0, 1.0e-6));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        float3 h = ptToWorld(float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta), n);
        l = reflect(-v, h);
        if (dot(n, l) <= 1.0e-5) return false;
    } else {
        float ignoredPdf; l = ptSampleCosine(n, rng, ignoredPdf);
    }
    float3 fd, fs; float pd, pg;
    ptEvalBsdf(base, metallic, roughness, n, v, l, fd, fs, pd, pg, pdf);
    value = fd + fs;
    return pdf > 1.0e-6;
}
float ptSunCosine(const constant PtParams& p) { return cos(max(p.sunColorRadius.w, 0.00001)); }
float3 ptSunDirection(const constant PtParams& p) {
    return ptSafeNormalize(-p.sunDirectionIntensity.xyz, float3(0.0, 1.0, 0.0));
}
bool ptInSunDisk(float3 direction, const constant PtParams& p) {
    return dot(direction, ptSunDirection(p)) >= ptSunCosine(p);
}
float ptSunPdf(float3 direction, const constant PtParams& p) {
    float c = ptSunCosine(p);
    return ptInSunDisk(direction, p) ? 1.0 / max(PT_TWO_PI * (1.0 - c), 1.0e-8) : 0.0;
}
float3 ptSunRadiance(const constant PtParams& p) {
    float radius = max(p.sunColorRadius.w, 0.00001);
    float projectedSolidAngle = PT_PI * sin(radius) * sin(radius);
    return max(p.sunColorRadius.xyz, float3(0.0)) * max(p.sunDirectionIntensity.w, 0.0) /
           max(projectedSolidAngle, 1.0e-8);
}
float3 ptSkyRadiance(float3 direction, const constant PtParams& p) {
    float y = max(direction.y, 0.0);
    float3 tint = mix(float3(0.78, 0.83, 0.90), max(p.skyColorIntensity.xyz, float3(0.0)), sqrt(y));
    float horizon = 1.0 - clamp(-p.sunDirectionIntensity.y, 0.0, 1.0);
    tint += max(p.sunColorRadius.xyz, float3(0.0)) * pow(max(dot(direction, ptSunDirection(p)), 0.0), 24.0) *
            (0.25 + 0.75 * horizon);
    return tint * max(p.skyColorIntensity.w, 0.0) / PT_PI * mix(0.25, 1.0, smoothstep(-0.12, 0.18, direction.y));
}
float3 ptSkyOnly(float3 direction, const constant PtParams& p) {
    return ptInSunDisk(direction, p) ? float3(0.0) : ptSkyRadiance(direction, p);
}
float ptSkyPdf(float3 direction, float3 n, const constant PtParams& p) {
    if (ptInSunDisk(direction, p)) return 0.0;
    return max(dot(n, direction), 0.0) / PT_PI;
}
float3 ptUnproject(float2 uv, float z, float4x4 inverseVP) {
    float4 q = inverseVP * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    float w = q.w;
    if (abs(w) < 1.0e-8) w = w < 0.0 ? -1.0e-8 : 1.0e-8;
    return q.xyz / w;
}
float ptLuminance(float3 c) { return dot(max(c, float3(0.0)), float3(0.2126, 0.7152, 0.0722)); }
float3 ptAces(float3 c) {
    c = max(c, float3(0.0));
    return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
}
float3 ptLinearToSrgb(float3 c) {
    c = max(c, float3(0.0));
    return mix(12.92 * c, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055,
               step(float3(0.0031308), c));
}
