#include "render.hpp"
#include "render_pt.hpp"
#include "render_raster.hpp"
#include <SDL3/SDL.h>
#include <stdexcept>
namespace aspera {
struct Renderer::Impl {
    std::unique_ptr<RasterRenderer> raster;
    std::unique_ptr<PathTracer> pt;
    RenderQuality quality{RenderQuality::Balanced};
    RenderDebug debug{RenderDebug::Final};
    std::string fallback;
};
Renderer::Renderer() : p(std::make_unique<Impl>()) {}
Renderer::~Renderer() {
    shutdown();
}
bool Renderer::init(SDL_Window *w, uint32_t width, uint32_t height, const AppOptions &o) {
    shutdown();
    if (o.pathTracing) {
        auto pt = std::make_unique<PathTracer>();
        if (pt->init(w, width, height,
                     {uint32_t(o.ptSamples), uint32_t(o.ptBounces), uint32_t(o.ptCaptureSamples),
                      !o.ptNoDenoise},
                     p->fallback)) {
            p->pt = std::move(pt);
            return true;
        }
        SDL_Log("Path tracing unavailable: %s; using raster", p->fallback.c_str());
        if (!o.renderBenchmark.empty() || o.ptCaptureSamples)
            throw std::runtime_error("Requested path-tracing capture/benchmark unavailable: " +
                                     p->fallback);
    }
    p->raster = std::make_unique<RasterRenderer>();
    return p->raster->init(w, width, height);
}
void Renderer::shutdown() {
    p->pt.reset();
    if (p->raster) {
        p->raster->shutdown();
        p->raster.reset();
    }
}
void Renderer::resize(uint32_t w, uint32_t h) {
    if (p->pt)
        p->pt->resize(w, h);
    else
        p->raster->resize(w, h);
}
void Renderer::begin(Vec3 e, const float *v) {
    if (p->pt)
        p->pt->begin(e, v);
    else
        p->raster->begin(e, v);
}
void Renderer::drawWorld(const World &w, bool s, const std::map<PlayerId, PlayerState> &players,
                         PlayerId local) {
    if (p->pt)
        p->pt->drawWorld(w, s, players, local);
    else
        p->raster->drawWorld(w, s, players, local);
}
void Renderer::drawImGui(ImDrawData *d) {
    if (p->pt)
        p->pt->drawImGui(d);
    else
        p->raster->drawImGui(d);
}
void Renderer::invalidateMeshes() {
    if (p->pt)
        p->pt->invalidateMeshes();
    else
        p->raster->invalidateMeshes();
}
void Renderer::setQuality(RenderQuality q) {
    p->quality = q;
    if (p->raster)
        p->raster->setQuality(q);
}
RenderQuality Renderer::quality() const {
    return p->quality;
}
void Renderer::setDebug(RenderDebug d) {
    p->debug = d;
    if (p->pt)
        p->pt->setDebug(d);
    else
        p->raster->setDebug(d);
}
RenderDebug Renderer::debug() const {
    return p->debug;
}
void Renderer::setBloom(bool b) {
    if (p->pt)
        p->pt->setBloom(b);
    else
        p->raster->setBloom(b);
}
void Renderer::setVsync(bool b) {
    if (p->pt)
        p->pt->setVsync(b);
    else
        p->raster->setVsync(b);
}
RenderTimings Renderer::timings() const {
    auto s = p->pt ? p->pt->timings() : p->raster->timings();
    s.fallbackReason = p->fallback;
    return s;
}
void Renderer::end() {
    if (p->pt)
        p->pt->end();
    else
        p->raster->end();
}
void Renderer::screenshot(const std::filesystem::path &f) {
    if (p->pt)
        p->pt->screenshot(f);
    else
        p->raster->screenshot(f);
}
bool Renderer::pathTraced() const {
    return bool(p->pt);
}
uint32_t Renderer::accumulatedSamples() const {
    return p->pt ? p->pt->accumulatedSamples() : 0;
}
} // namespace aspera
