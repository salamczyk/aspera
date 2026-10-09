#include "render_pt.hpp"
#include "render_scene.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <stdexcept>
namespace aspera {
std::filesystem::path ptShaderPath(const char *name) {
    std::vector<std::filesystem::path> roots;
    if (auto base = SDL_GetBasePath()) {
        roots.emplace_back(std::filesystem::path(base) / "shaders/pt");
        roots.emplace_back(std::filesystem::path(base) / "../share/aspera/shaders/pt");
    }
#ifdef ASPERA_SHADER_DIR
    roots.emplace_back(std::filesystem::path(ASPERA_SHADER_DIR) / "pt");
#endif
    roots.emplace_back("assets/shaders/pt");
    for (auto &r : roots)
        if (std::filesystem::exists(r / name))
            return r / name;
    throw std::runtime_error(std::string("Missing path-tracing shader: ") + name);
}
#if !defined(__APPLE__)
std::unique_ptr<NativePt> makeMetalPt() {
    return {};
}
#endif
#if !defined(ASPERA_HAS_VULKAN_PT)
std::unique_ptr<NativePt> makeVulkanPt() {
    return {};
}
#endif
struct PathTracer::Impl {
    std::unique_ptr<NativePt> native;
    RenderSceneBuilder builder;
    PtSettings settings;
    PtParams params{};
    uint32_t width{}, height{}, accumulated{}, sequence{};
    uint64_t geometry{}, transforms{}, lighting{};
    bool reset{true}, bloom{true}, hasPrevious{};
    RenderDebug debug{RenderDebug::Final};
    float vp[16]{}, previousVP[16]{};
    struct Line {
        ImVec2 a, b;
        ImU32 color;
    };
    std::vector<Line> lines;
    std::unique_ptr<ImDrawList> overlay;
    ImDrawData merged;
};
PathTracer::PathTracer() : p(std::make_unique<Impl>()) {}
PathTracer::~PathTracer() = default;
bool PathTracer::init(SDL_Window *w, uint32_t width, uint32_t height, const PtSettings &s,
                      std::string &reason) {
    p->settings = s;
    p->width = width;
    p->height = height;
#if defined(__APPLE__)
    p->native = makeMetalPt();
#else
    p->native = makeVulkanPt();
#endif
    if (!p->native) {
        reason = "Native path-tracing backend was not built (Vulkan SDK/shader compiler required "
                 "on Windows/Linux)";
        return false;
    }
    if (!p->native->init(w, width, height, reason)) {
        p->native.reset();
        return false;
    }
    return true;
}
void PathTracer::resize(uint32_t w, uint32_t h) {
    p->width = std::max(1u, w);
    p->height = std::max(1u, h);
    p->native->resize(p->width, p->height);
    p->reset = true;
    p->accumulated = 0;
}
void PathTracer::begin(Vec3 eye, const float *view) {
    float projection[16];
    bx::mtxProj(projection, 60.f, float(p->width) / p->height, .05f, 1000.f, false);
    bx::mtxMul(p->vp, view, projection);
    bx::mtxInverse(p->params.inverseVP, p->vp);
    std::copy(p->vp, p->vp + 16, p->params.viewProjection);
    std::copy(p->hasPrevious ? p->previousVP : p->vp,
              p->hasPrevious ? p->previousVP + 16 : p->vp + 16, p->params.previousVP);
    p->params.eye[0] = eye.x;
    p->params.eye[1] = eye.y;
    p->params.eye[2] = eye.z;
}
void PathTracer::drawWorld(const World &w, bool stress,
                           const std::map<PlayerId, PlayerState> &players, PlayerId local) {
    p->lines.clear();
    if (stress) {
        constexpr unsigned edges[]{0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7,
                                   7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7};
        size_t remaining = 4096;
        for (auto &[id, o] : w.objects) {
            for (auto &r : o.stressRegions) {
                if (!remaining)
                    break;
                --remaining;
                ImVec2 corners[8];
                bool valid[8]{};
                for (unsigned i = 0; i < 8; i++) {
                    Vec3 a{i & 1 ? r.maximum.x : r.minimum.x, i & 2 ? r.maximum.y : r.minimum.y,
                           i & 4 ? r.maximum.z : r.minimum.z};
                    a = rotate(o.transform.rotation, a) + o.transform.position;
                    float clip[4]{};
                    for (int j = 0; j < 4; j++)
                        clip[j] = p->vp[j] * a.x + p->vp[4 + j] * a.y + p->vp[8 + j] * a.z +
                                  p->vp[12 + j];
                    if (clip[3] > .05f) {
                        valid[i] = true;
                        corners[i] = {(clip[0] / clip[3] * .5f + .5f) * p->width,
                                      (.5f - clip[1] / clip[3] * .5f) * p->height};
                    }
                }
                uint32_t packed = stressColor(r.utilization);
                ImU32 color = IM_COL32(packed >> 24, (packed >> 16) & 255, (packed >> 8) & 255, 200);
                for (int i = 0; i < 24; i += 2)
                    if (valid[edges[i]] && valid[edges[i + 1]])
                        p->lines.push_back({corners[edges[i]], corners[edges[i + 1]], color});
            }
            if (!remaining)
                break;
        }
    }
    const auto &s = p->builder.update(w, players, local);
    auto &f = p->params;
    bool changed =
        p->reset || s.geometryRevision != p->geometry || s.lightingRevision != p->lighting;
    bool stationary = p->hasPrevious && !changed && s.transformRevision == p->transforms &&
                      std::memcmp(p->vp, p->previousVP, sizeof(p->vp)) == 0 && p->builder.ready();
    if (changed || !stationary)
        p->accumulated = 0;
    if (changed)
        p->sequence = 0;
    auto sun = normalized(w.environment.sunDirection);
    f.sunDirectionIntensity[0] = sun.x;
    f.sunDirectionIntensity[1] = sun.y;
    f.sunDirectionIntensity[2] = sun.z;
    f.sunDirectionIntensity[3] = w.environment.sunIntensity;
    f.sunColorRadius[0] = w.environment.sunColor.x;
    f.sunColorRadius[1] = w.environment.sunColor.y;
    f.sunColorRadius[2] = w.environment.sunColor.z;
    f.sunColorRadius[3] = .27f * .01745329252f;
    f.skyColorIntensity[0] = w.environment.skyColor.x;
    f.skyColorIntensity[1] = w.environment.skyColor.y;
    f.skyColorIntensity[2] = w.environment.skyColor.z;
    f.skyColorIntensity[3] = w.environment.skyIntensity;
    f.post[0] = 1 / (1.2f * std::exp2(w.environment.exposure));
    f.post[1] = p->bloom ? w.environment.bloomStrength : 0;
    f.post[2] = float(p->debug);
    uint32_t samples = p->settings.samples;
    if (p->settings.captureSamples) {
        if (!p->builder.ready()) {
            p->accumulated = 0;
            p->sequence = 0;
            changed = true;
        }
        samples = std::min(samples, p->settings.captureSamples - p->accumulated);
        stationary = p->builder.ready();
    }
    f.sizeFrame[0] = p->width;
    f.sizeFrame[1] = p->height;
    f.sizeFrame[2] = p->sequence;
    f.sizeFrame[3] = changed ? 1 : 0;
    f.counts[0] = samples;
    f.counts[1] = p->settings.bounces;
    f.counts[2] = uint32_t(s.lights.size());
    f.counts[3] = uint32_t(s.emitters.size());
    f.options[0] = p->settings.denoise && !p->settings.captureSamples;
    f.options[1] = stationary;
    f.options[2] = p->accumulated;
    f.options[3] = uint32_t(s.instances.size());
    if (!p->native->render(s, f)) {
        p->reset = true;
        p->accumulated = 0;
        return;
    }
    p->sequence += samples;
    if (p->builder.ready())
        p->accumulated += samples;
    p->geometry = s.geometryRevision;
    p->lighting = s.lightingRevision;
    p->transforms = s.transformRevision;
    p->reset = false;
    std::copy(p->vp, p->vp + 16, p->previousVP);
    p->hasPrevious = true;
}
void PathTracer::drawImGui(ImDrawData *d) {
    if (p->lines.empty() || !d) {
        p->native->drawImGui(d);
        return;
    }
    p->overlay = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
    p->overlay->_ResetForNewFrame();
    p->overlay->PushTextureID(ImGui::GetIO().Fonts->TexID);
    p->overlay->PushClipRectFullScreen();
    auto logical = [&](ImVec2 a) {
        return ImVec2(a.x / d->FramebufferScale.x + d->DisplayPos.x,
                      a.y / d->FramebufferScale.y + d->DisplayPos.y);
    };
    for (auto &l : p->lines)
        p->overlay->AddLine(logical(l.a), logical(l.b), l.color, 1);
    p->overlay->PopClipRect();
    p->overlay->PopTextureID();
    p->merged = *d;
    p->merged.AddDrawList(p->overlay.get());
    p->native->drawImGui(&p->merged);
}
void PathTracer::invalidateMeshes() {
    p->builder.invalidate();
    p->reset = true;
    p->accumulated = 0;
}
void PathTracer::setBloom(bool b) {
    p->bloom = b;
}
void PathTracer::setVsync(bool b) {
    p->native->setVsync(b);
}
void PathTracer::setDebug(RenderDebug d) {
    p->debug = d;
}
RenderTimings PathTracer::timings() const {
    auto s = p->native->timings();
    p->builder.statistics(s);
    s.ptSamples = p->settings.samples;
    s.ptBounces = p->settings.bounces;
    s.accumulatedSamples = p->accumulated;
    return s;
}
void PathTracer::end() {
    p->native->end();
}
void PathTracer::screenshot(const std::filesystem::path &f) {
    p->native->screenshot(f);
}
uint32_t PathTracer::accumulatedSamples() const {
    return p->accumulated;
}
} // namespace aspera
