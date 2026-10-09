#include "render_pt.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
namespace aspera {
namespace {
constexpr const char *MetalUiBackendName = "aspera_metal_path_tracer";
void check(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
std::string readSource(const char *name) {
    std::ifstream f(ptShaderPath(name));
    check(bool(f), "Cannot read Metal path-tracing shader");
    return {std::istreambuf_iterator<char>(f), {}};
}
struct MetalChunk {
    uint64_t key{};
    id<MTLBuffer> vertices = nil, indices = nil;
    id<MTLAccelerationStructure> as = nil;
};
class MetalPt final : public NativePt {
    SDL_Window *window{};
    SDL_MetalView view{};
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer *layer = nil;
    std::array<id<MTLComputePipelineState>, 4> compute{};
    id<MTLRenderPipelineState> present = nil, ui = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> font = nil;
    std::array<id<MTLTexture>, PtImageCount> images{};
    std::array<id<MTLBuffer>, 7> buffers{};
    std::map<uint64_t, MetalChunk> chunks;
    id<MTLAccelerationStructure> tlas = nil;
    id<MTLBuffer> instanceDescriptors = nil;
    std::vector<id<MTLCommandBuffer>> commands;
    id<MTLCommandBuffer> asCommand = nil, traceCommand = nil, denoiseCommand = nil;
    std::vector<id<MTLBuffer>> frameBuffers;
    uint32_t width{}, height{}, frame{};
    uint64_t geometryRevision{UINT64_MAX}, transformRevision{UINT64_MAX};
    RenderTimings stats;
    std::filesystem::path capture;
    ImDrawData *drawData{};
    PtParams params{};
    std::chrono::steady_clock::time_point start;
    id<MTLBuffer> buffer(const void *p, size_t size) {
        auto b = [device newBufferWithLength:std::max(size, size_t(256))
                                     options:MTLResourceStorageModeShared];
        check(b != nil, "Metal path-tracing buffer allocation failed");
        if (size && p)
            std::memcpy(b.contents, p, size);
        return b;
    }
    id<MTLLibrary> library(const char *name) {
        auto base = std::string(name);
        std::filesystem::path compiled;
        try {
            compiled = ptShaderPath((base + ".metallib").c_str());
        } catch (const std::exception &) {
        }
        NSError *error = nil;
        id<MTLLibrary> lib = nil;
        if (!compiled.empty())
            lib = [device
                newLibraryWithURL:[NSURL fileURLWithPath:[NSString
                                                             stringWithUTF8String:compiled.c_str()]]
                            error:&error];
        if (!lib) {
            auto text = readSource((base + ".metal").c_str());
            auto token = std::string("#include \"pt_common.metal\"");
            auto at = text.find(token);
            if (at != std::string::npos)
                text.replace(at, token.size(), readSource("pt_common.metal"));
            MTLCompileOptions *o = [MTLCompileOptions new];
            o.languageVersion = MTLLanguageVersion2_4;
            lib = [device newLibraryWithSource:[NSString stringWithUTF8String:text.c_str()]
                                       options:o
                                         error:&error];
        }
        if (!lib)
            throw std::runtime_error(
                "Metal shader " + base + ": " +
                (error ? std::string(error.localizedDescription.UTF8String) : "unknown error"));
        return lib;
    }
    void submit(id<MTLCommandBuffer> c) {
        [c commit];
        commands.push_back(c);
    }
    void wait() {
        for (auto c : commands) {
            [c waitUntilCompleted];
            if (c.status == MTLCommandBufferStatusError)
                throw std::runtime_error("Metal path-tracing command failed: " +
                                         std::string(c.error.localizedDescription.UTF8String));
        }
        commands.clear();
        frameBuffers.clear();
    }
    void allocate() {
        auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                    width:width
                                                                   height:height
                                                                mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        for (auto &t : images) {
            t = [device newTextureWithDescriptor:d];
            check(t != nil, "Metal path-tracing image allocation failed");
        }
        // Initialize histories before shaders may access them, even on reset frames.
        auto c = [queue commandBuffer];
        auto b = [c blitCommandEncoder];
        auto zero = buffer(nullptr, size_t(width) * height * 16);
        std::memset(zero.contents, 0, zero.length);
        for (auto t : images)
            [b copyFromBuffer:zero
                       sourceOffset:0
                  sourceBytesPerRow:width * 16
                sourceBytesPerImage:width * height * 16
                         sourceSize:MTLSizeMake(width, height, 1)
                          toTexture:t
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        submit(c);
        wait();
    }
    id<MTLRenderPipelineState> renderPipeline(id<MTLLibrary> lib, const char *v, const char *f,
                                              bool blend) {
        auto d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:v]];
        d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithUTF8String:f]];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (blend) {
            auto vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = MTLVertexFormatFloat2;
            vd.attributes[0].offset = offsetof(ImDrawVert, pos);
            vd.attributes[0].bufferIndex = 0;
            vd.attributes[1].format = MTLVertexFormatFloat2;
            vd.attributes[1].offset = offsetof(ImDrawVert, uv);
            vd.attributes[1].bufferIndex = 0;
            vd.attributes[2].format = MTLVertexFormatUChar4;
            vd.attributes[2].offset = offsetof(ImDrawVert, col);
            vd.attributes[2].bufferIndex = 0;
            vd.layouts[0].stride = sizeof(ImDrawVert);
            d.vertexDescriptor = vd;
        }
        if (blend) {
            auto a = d.colorAttachments[0];
            a.blendingEnabled = YES;
            a.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            a.sourceAlphaBlendFactor = MTLBlendFactorOne;
            a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError *error = nil;
        auto r = [device newRenderPipelineStateWithDescriptor:d error:&error];
        if (!r)
            throw std::runtime_error(
                std::string("Metal presentation pipeline: ") +
                (error ? error.localizedDescription.UTF8String : "missing functions"));
        return r;
    }
    MTLPrimitiveAccelerationStructureDescriptor *descriptor(MetalChunk &chunk, uint32_t count) {
        auto g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        g.vertexBuffer = chunk.vertices;
        g.vertexStride = sizeof(PtVertex);
        g.indexBuffer = chunk.indices;
        g.indexType = MTLIndexTypeUInt32;
        g.triangleCount = count / 3;
        g.opaque = YES;
        auto d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        d.geometryDescriptors = @[ g ];
        return d;
    }
    void scene(const PtScene &s) {
        bool uploadGeometry = geometryRevision != s.geometryRevision;
        auto c = [queue commandBuffer];
        auto enc = [c accelerationStructureCommandEncoder];
        bool built = false;
        NSMutableArray<id<MTLAccelerationStructure>> *structures = [NSMutableArray new];
        std::set<uint64_t> live;
        for (auto &g : s.geometry) {
            live.insert(g.id);
            auto i = chunks.find(g.id);
            if (i == chunks.end() || i->second.key != g.key) {
                MetalChunk n;
                n.key = g.key;
                n.vertices = buffer(s.vertices.data() + g.vertexOffset,
                                    size_t(g.vertexCount) * sizeof(PtVertex));
                n.indices = buffer(s.indices.data() + g.indexOffset, size_t(g.indexCount) * 4);
                auto d = descriptor(n, g.indexCount);
                auto sizes = [device accelerationStructureSizesWithDescriptor:d];
                n.as = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
                check(n.as != nil, "Metal BLAS allocation failed");
                auto scratch = buffer(nullptr, sizes.buildScratchBufferSize);
                frameBuffers.push_back(scratch);
                [enc buildAccelerationStructure:n.as
                                     descriptor:d
                                  scratchBuffer:scratch
                            scratchBufferOffset:0];
                chunks[g.id] = n;
                built = true;
            }
            [structures addObject:chunks.at(g.id).as];
        }
        // A valid masked dummy TLAS also supports sky-only scenes.
        if (structures.count == 0) {
            auto &n = chunks[0];
            if (!n.as) {
                PtVertex v[3]{};
                v[1].position[0] = .01f;
                v[2].position[1] = .01f;
                uint32_t ix[]{0, 1, 2};
                n.vertices = buffer(v, sizeof(v));
                n.indices = buffer(ix, sizeof(ix));
                auto d = descriptor(n, 3);
                auto sz = [device accelerationStructureSizesWithDescriptor:d];
                n.as = [device newAccelerationStructureWithSize:sz.accelerationStructureSize];
                auto scratch = buffer(nullptr, sz.buildScratchBufferSize);
                frameBuffers.push_back(scratch);
                [enc buildAccelerationStructure:n.as
                                     descriptor:d
                                  scratchBuffer:scratch
                            scratchBufferOffset:0];
                built = true;
            }
            [structures addObject:n.as];
        }
        bool rebuild = geometryRevision != s.geometryRevision ||
                       transformRevision != s.transformRevision || !tlas;
        if (rebuild) {
            std::vector<MTLAccelerationStructureInstanceDescriptor> desc(
                std::max(size_t(1), s.instances.size()));
            for (size_t i = 0; i < desc.size(); i++) {
                auto &d = desc[i];
                d.mask = s.instances.empty() ? 0 : 0xff;
                d.options = MTLAccelerationStructureInstanceOptionOpaque;
                d.accelerationStructureIndex = s.instances.empty() ? 0 : s.instances[i].offsets[3];
                float identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                const float *m = s.instances.empty() ? identity : s.instances[i].model;
                std::memcpy(&d.transformationMatrix.columns[0], m, 12);
                std::memcpy(&d.transformationMatrix.columns[1], m + 4, 12);
                std::memcpy(&d.transformationMatrix.columns[2], m + 8, 12);
                std::memcpy(&d.transformationMatrix.columns[3], m + 12, 12);
            }
            instanceDescriptors = buffer(desc.data(), desc.size() * sizeof(desc[0]));
            auto d = [MTLInstanceAccelerationStructureDescriptor descriptor];
            d.instancedAccelerationStructures = structures;
            d.instanceCount = desc.size();
            d.instanceDescriptorBuffer = instanceDescriptors;
            d.usage = MTLAccelerationStructureUsageRefit;
            auto sz = [device accelerationStructureSizesWithDescriptor:d];
            bool refit = tlas && geometryRevision == s.geometryRevision &&
                         tlas.size >= sz.accelerationStructureSize;
            if (!refit) {
                tlas = [device newAccelerationStructureWithSize:sz.accelerationStructureSize];
                check(tlas != nil, "Metal TLAS allocation failed");
            }
            auto scratch =
                buffer(nullptr, refit ? sz.refitScratchBufferSize : sz.buildScratchBufferSize);
            frameBuffers.push_back(scratch);
            if (refit)
                [enc refitAccelerationStructure:tlas
                                     descriptor:d
                                    destination:tlas
                                  scratchBuffer:scratch
                            scratchBufferOffset:0];
            else
                [enc buildAccelerationStructure:tlas
                                     descriptor:d
                                  scratchBuffer:scratch
                            scratchBufferOffset:0];
            built = true;
        }
        [enc endEncoding];
        asCommand = built ? c : nil;
        submit(c);
        for (auto i = chunks.begin(); i != chunks.end();)
            if (!live.contains(i->first) && i->first != 0)
                i = chunks.erase(i);
            else
                ++i;
        geometryRevision = s.geometryRevision;
        transformRevision = s.transformRevision;
        if (uploadGeometry) {
            buffers[1] = buffer(s.vertices.data(), s.vertices.size() * sizeof(PtVertex));
            buffers[2] = buffer(s.indices.data(), s.indices.size() * 4);
        }
        buffers[3] = buffer(s.instances.data(), s.instances.size() * sizeof(PtInstance));
        buffers[4] = buffer(s.materials.data(), s.materials.size() * sizeof(PtMaterial));
        buffers[5] = buffer(s.lights.data(), s.lights.size() * sizeof(PtLight));
        buffers[6] = buffer(s.emitters.data(), s.emitters.size() * sizeof(PtEmitter));
    }
    void dispatch(id<MTLCommandBuffer> c, unsigned pipeline, uint32_t pass = 0) {
        auto e = [c computeCommandEncoder];
        [e setComputePipelineState:compute[pipeline]];
        for (unsigned i = 0; i < 7; i++)
            [e setBuffer:buffers[i] offset:0 atIndex:i];
        for (unsigned i = 0; i < PtImageCount; i++)
            [e setTexture:images[i] atIndex:i + 8];
        if (pipeline == 0) {
            [e setAccelerationStructure:tlas atBufferIndex:7];
            for (auto &[id, ch] : chunks)
                [e useResource:ch.as usage:MTLResourceUsageRead];
        }
        if (pipeline == 2)
            [e setBytes:&pass length:4 atIndex:8];
        [e dispatchThreadgroups:MTLSizeMake((width + 7) / 8, (height + 7) / 8, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [e endEncoding];
    }
    void history(id<MTLCommandBuffer> c) {
        auto b = [c blitCommandEncoder];
        const unsigned src[]{PtHistoryDiffuse, PtHistorySpecular, PtHistoryGuide, PtHistoryMotion,
                             PtMoments},
            dst[]{PtPreviousDiffuse, PtPreviousSpecular, PtPreviousGuide, PtPreviousMotion,
                  PtPreviousMoments};
        for (unsigned i = 0; i < 5; i++)
            [b copyFromTexture:images[src[i]]
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                        toTexture:images[dst[i]]
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }

  public:
    ~MetalPt() override {
        @autoreleasepool {
            for (auto c : commands)
                [c waitUntilCompleted];
            if (ImGui::GetCurrentContext() &&
                ImGui::GetIO().BackendRendererName == MetalUiBackendName) {
                ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
                ImGui::GetIO().BackendRendererName = nullptr;
            }
            if (view)
                SDL_Metal_DestroyView(view);
        }
    }
    bool init(SDL_Window *w, uint32_t x, uint32_t y, std::string &reason) override {
        @autoreleasepool {
            try {
                if (@available(macOS 11.0, *)) {
                } else {
                    reason = "Metal ray tracing requires macOS 11 or newer";
                    return false;
                }
                window = w;
                width = std::max(1u, x);
                height = std::max(1u, y);
                device = MTLCreateSystemDefaultDevice();
                if (!device || !device.supportsRaytracing) {
                    reason = "Metal device does not support ray tracing";
                    return false;
                }
                queue = [device newCommandQueue];
                check(queue != nil, "Metal command queue allocation failed");
                view = SDL_Metal_CreateView(w);
                check(view != nullptr, "SDL Metal view creation failed");
                layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
                layer.device = device;
                layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer.framebufferOnly = NO;
                layer.drawableSize = CGSizeMake(width, height);
                const char *names[]{"pt_trace", "pt_temporal", "pt_filter", "pt_post"};
                for (unsigned i = 0; i < 4; i++) {
                    auto lib = library(names[i]);
                    auto fn = [lib newFunctionWithName:[NSString stringWithUTF8String:names[i]]];
                    NSError *err = nil;
                    compute[i] = [device newComputePipelineStateWithFunction:fn error:&err];
                    if (!compute[i])
                        throw std::runtime_error(
                            std::string("Metal compute pipeline: ") +
                            (err ? err.localizedDescription.UTF8String : names[i]));
                }
                auto pl = library("pt_present");
                present = renderPipeline(pl, "pt_present_vertex", "pt_present_fragment", false);
                auto ul = library("pt_imgui");
                ui = renderPipeline(ul, "pt_imgui_vertex", "pt_imgui_fragment", true);
                auto sd = [MTLSamplerDescriptor new];
                sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
                sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
                sampler = [device newSamplerStateWithDescriptor:sd];
                unsigned char *pixels;
                int fw, fh;
                ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
                auto fd = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                 width:fw
                                                height:fh
                                             mipmapped:NO];
                fd.usage = MTLTextureUsageShaderRead;
                font = [device newTextureWithDescriptor:fd];
                check(font != nil, "Metal UI font allocation failed");
                [font replaceRegion:MTLRegionMake2D(0, 0, fw, fh)
                        mipmapLevel:0
                          withBytes:pixels
                        bytesPerRow:fw * 4];
                ImGui::GetIO().Fonts->SetTexID((ImTextureID)1);
                allocate();
                ImGui::GetIO().BackendRendererName = MetalUiBackendName;
                ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
                stats.backend = "path-traced (Metal)";
                reason.clear();
                return true;
            } catch (const std::exception &e) {
                reason = e.what();
                return false;
            }
        }
    }
    void resize(uint32_t x, uint32_t y) override {
        @autoreleasepool {
            wait();
            width = std::max(1u, x);
            height = std::max(1u, y);
            layer.drawableSize = CGSizeMake(width, height);
            allocate();
        }
    }
    bool render(const PtScene &s, const PtParams &f) override {
        @autoreleasepool {
            start = std::chrono::steady_clock::now();
            wait();
            params = f;
            scene(s);
            buffers[0] = buffer(&f, sizeof(f));
            traceCommand = [queue commandBuffer];
            dispatch(traceCommand, 0);
            submit(traceCommand);
            denoiseCommand = [queue commandBuffer];
            dispatch(denoiseCommand, 1);
            history(denoiseCommand);
            for (unsigned i = 0; i < 5; i++)
                dispatch(denoiseCommand, 2, i);
            dispatch(denoiseCommand, 3);
            submit(denoiseCommand);
            drawData = nullptr;
            stats.geometryBytes = 0;
            for (auto &[id, ch] : chunks)
                stats.geometryBytes += ch.vertices.length + ch.indices.length + ch.as.size;
            stats.ptBytes = stats.geometryBytes + tlas.size + instanceDescriptors.length;
            for (auto b : buffers)
                stats.ptBytes += b.length;
            stats.textureBytes = uint64_t(width) * height * 16 * PtImageCount;
            stats.ptBytes += stats.textureBytes;
            return true;
        }
    }
    void drawImGui(ImDrawData *d) override {
        drawData = d;
    }
    void end() override {
        @autoreleasepool {
            auto drawable = [layer nextDrawable];
            auto c = [queue commandBuffer];
            id<MTLBuffer> readback = nil;
            if (drawable) {
                auto d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = drawable.texture;
                d.colorAttachments[0].loadAction = MTLLoadActionClear;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                auto e = [c renderCommandEncoderWithDescriptor:d];
                [e setRenderPipelineState:present];
                [e setFragmentTexture:images[PtOutput] atIndex:0];
                [e setFragmentSamplerState:sampler atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                if (drawData && drawData->TotalVtxCount) {
                    float L = drawData->DisplayPos.x, R = L + drawData->DisplaySize.x,
                          T = drawData->DisplayPos.y, B = T + drawData->DisplaySize.y;
                    float projection[16]{
                        2 / (R - L), 0, 0, 0,   0, 2 / (T - B),       0,
                        0,           0, 0, .5f, 0, (R + L) / (L - R), (T + B) / (B - T),
                        .5f,         1};
                    [e setRenderPipelineState:ui];
                    [e setVertexBytes:projection length:sizeof(projection) atIndex:2];
                    [e setFragmentTexture:font atIndex:0];
                    [e setFragmentSamplerState:sampler atIndex:0];
                    [e setCullMode:MTLCullModeNone];
                    for (int n = 0; n < drawData->CmdListsCount; n++) {
                        auto list = drawData->CmdLists[n];
                        auto vb =
                            buffer(list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
                        auto ib =
                            buffer(list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
                        frameBuffers.push_back(vb);
                        frameBuffers.push_back(ib);
                        for (auto &cmd : list->CmdBuffer) {
                            if (cmd.UserCallback) {
                                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                                    cmd.UserCallback(list, &cmd);
                                continue;
                            }
                            float sx = (cmd.ClipRect.x - L) * drawData->FramebufferScale.x,
                                  sy = (cmd.ClipRect.y - T) * drawData->FramebufferScale.y,
                                  ex = (cmd.ClipRect.z - L) * drawData->FramebufferScale.x,
                                  ey = (cmd.ClipRect.w - T) * drawData->FramebufferScale.y;
                            sx = std::clamp(sx, 0.f, float(width));
                            sy = std::clamp(sy, 0.f, float(height));
                            ex = std::clamp(ex, 0.f, float(width));
                            ey = std::clamp(ey, 0.f, float(height));
                            if (ex <= sx || ey <= sy)
                                continue;
                            [e setScissorRect:MTLScissorRect{NSUInteger(sx), NSUInteger(sy),
                                                             NSUInteger(ex - sx),
                                                             NSUInteger(ey - sy)}];
                            [e setVertexBuffer:vb
                                        offset:cmd.VtxOffset * sizeof(ImDrawVert)
                                       atIndex:0];
                            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                          indexCount:cmd.ElemCount
                                           indexType:sizeof(ImDrawIdx) == 2 ? MTLIndexTypeUInt16
                                                                            : MTLIndexTypeUInt32
                                         indexBuffer:ib
                                   indexBufferOffset:cmd.IdxOffset * sizeof(ImDrawIdx)];
                        }
                    }
                }
                [e endEncoding];
                if (!capture.empty()) {
                    readback = buffer(nullptr, size_t(width) * height * 4);
                    auto b = [c blitCommandEncoder];
                    [b copyFromTexture:drawable.texture
                                     sourceSlice:0
                                     sourceLevel:0
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:MTLSizeMake(width, height, 1)
                                        toBuffer:readback
                               destinationOffset:0
                          destinationBytesPerRow:width * 4
                        destinationBytesPerImage:width * height * 4];
                    [b endEncoding];
                }
                [c presentDrawable:drawable];
            } else if (!capture.empty())
                throw std::runtime_error(
                    "Cannot capture path-traced image while drawable is unavailable");
            submit(c);
            wait();
            auto duration = [](id<MTLCommandBuffer> b) {
                return b ? std::max(0., (b.GPUEndTime - b.GPUStartTime) * 1000) : 0.;
            };
            stats.accelerationMs = duration(asCommand);
            stats.traceMs = duration(traceCommand);
            stats.denoiseMs = duration(denoiseCommand);
            // Tile GPU command intervals can overlap; use the frame envelope rather
            // than summing pass intervals (which double-counts overlap).
            double first = traceCommand.GPUStartTime;
            if (asCommand)
                first = std::min(first, asCommand.GPUStartTime);
            stats.gpuMs = std::max(0., (c.GPUEndTime - first) * 1000);
            stats.gpuFrame = ++frame;
            stats.cpuMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            stats.passes = {{201, "Path trace", 0, stats.traceMs, frame},
                            {202, "PT denoise and post", 0, stats.denoiseMs, frame},
                            {203, "Acceleration structures", 0, stats.accelerationMs, frame}};
            if (readback) {
                auto path = capture;
                path.replace_extension(".ppm");
                std::ofstream out(path, std::ios::binary);
                check(bool(out), "Cannot write path-tracing screenshot");
                out << "P6\n" << width << ' ' << height << "\n255\n";
                auto bytes = (const unsigned char *)readback.contents;
                for (size_t i = 0; i < size_t(width) * height; i++) {
                    char rgb[]{char(bytes[i * 4 + 2]), char(bytes[i * 4 + 1]), char(bytes[i * 4])};
                    out.write(rgb, 3);
                }
                check(bool(out), "Path-tracing screenshot write failed");
                capture.clear();
            }
        }
    }
    void screenshot(const std::filesystem::path &f) override {
        capture = f;
    }
    void setVsync(bool b) override {
        layer.displaySyncEnabled = b;
    }
    RenderTimings timings() const override {
        return stats;
    }
};
} // namespace
std::unique_ptr<NativePt> makeMetalPt() {
    return std::make_unique<MetalPt>();
}
} // namespace aspera
