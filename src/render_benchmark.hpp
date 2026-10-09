#pragma once
#include "render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace aspera {
class RenderBenchmark {
    struct Frame {
        double time{}, frameMs{}, simulationMs{};
        RenderTimings rendering;
    };
    std::vector<Frame> frames;
    AppOptions options;
    int width{}, height{};
    bool resolutionChanged{};
    static double percentile(std::vector<double> values, double fraction) {
        if (values.empty())
            return -1;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, size_t(std::ceil(fraction * values.size())) - 1)];
    }
    static std::string json(const std::string &text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\')
                out += '\\';
            if (c == '\n') {
                out += "\\n";
                continue;
            }
            out += c;
        }
        return out + '"';
    }

  public:
    RenderBenchmark(const AppOptions &opts, int pixelWidth, int pixelHeight)
        : options(opts), width(pixelWidth), height(pixelHeight) {}
    void record(double elapsed, double frameMs, double simulationMs, RenderTimings stats) {
        frames.push_back({elapsed, frameMs, simulationMs, std::move(stats)});
    }
    void resized(int pixelWidth, int pixelHeight) {
        resolutionChanged |= pixelWidth != width || pixelHeight != height;
        width = pixelWidth;
        height = pixelHeight;
    }
    void write() const {
        if (options.renderBenchmark.empty())
            return;
        auto path = options.renderBenchmark;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream csv(path);
        if (!csv)
            throw std::runtime_error("Could not write graphics benchmark");
        csv << "frame,time_seconds,frame_ms,submit_ms,simulation_ms,gpu_frame,gpu_ms,gi_frame,gi_"
               "ms,texture_"
               "bytes,gi_bytes,draw_calls,gi_updated_bricks,trace_ms,denoise_ms,as_ms,pt_bytes,pt_samples,pt_bounces\n"
            << std::fixed << std::setprecision(6);
        auto passPath = path;
        passPath.replace_extension("passes.csv");
        std::ofstream passes(passPath);
        passes << "gpu_frame,view_id,view_name,cpu_ms,gpu_ms\n"
               << std::fixed << std::setprecision(6);
        std::vector<double> wall, submit, simulation, gpu, gi, trace, denoise, acceleration;
        std::set<uint32_t> seen, seenGi;
        std::set<std::pair<uint32_t, uint16_t>> seenPasses;
        uint64_t texturePeak = 0, giPeak = 0, ptPeak = 0;
        bool nativePt = !frames.empty();
        for (size_t i = 0; i < frames.size(); ++i) {
            const auto &f = frames[i];
            const auto &s = f.rendering;
            csv << i << ',' << f.time << ',' << f.frameMs << ',' << s.cpuMs << ',' << f.simulationMs
                << ',' << s.gpuFrame << ',' << s.gpuMs << ',' << s.giFrame << ',' << s.giMs << ','
                << s.textureBytes << ',' << s.giBytes << ',' << s.drawCalls << ',' << s.pendingGi
                << ',' << s.traceMs << ',' << s.denoiseMs << ',' << s.accelerationMs << ',' << s.ptBytes << ',' << s.ptSamples << ',' << s.ptBounces << '\n';
            wall.push_back(f.frameMs);
            submit.push_back(s.cpuMs);
            simulation.push_back(f.simulationMs);
            texturePeak = std::max(texturePeak, s.textureBytes);
            giPeak = std::max(giPeak, s.giBytes);
            ptPeak = std::max(ptPeak,s.ptBytes);
            nativePt &= s.backend.find("path-traced") == 0;
            if(s.gpuMs>0 && !seen.contains(s.gpuFrame)){
                if(s.traceMs>0) trace.push_back(s.traceMs);
                if(s.denoiseMs>0) denoise.push_back(s.denoiseMs);
                acceleration.push_back(s.accelerationMs);
            }
            if (s.gpuMs > 0 && seen.insert(s.gpuFrame).second)
                gpu.push_back(s.gpuMs);
            if (s.giMs > 0 && seenGi.insert(s.giFrame).second)
                gi.push_back(s.giMs);
            for (auto &p : s.passes)
                if (p.gpuMs > 0 && seenPasses.insert({p.gpuFrame, p.id}).second)
                    passes << p.gpuFrame << ',' << p.id << ',' << json(p.name) << ',' << p.cpuMs
                           << ',' << p.gpuMs << '\n';
        }
        auto summary = path;
        summary.replace_extension("json");
        std::ofstream out(summary);
        out << std::fixed << std::setprecision(6);
        const double measuredSeconds =
            frames.empty() ? 0 : frames.back().time - frames.front().time;
        const size_t requiredTimingSamples = std::max(size_t(120), (frames.size() + 1) / 2);
        bool gate = width == 1920 && height == 1080 && !resolutionChanged && !options.lowQuality &&
                    !options.noBloom && options.debugView == 0 && options.benchmarkSeconds >= 60 &&
                    frames.size() >= 60 && measuredSeconds >= options.benchmarkSeconds - .1 &&
                    percentile(wall, .95) <= 16.7 && gpu.size() >= requiredTimingSamples &&
                    gi.size() >= requiredTimingSamples;
        if(options.pathTracing)
            gate = nativePt && width==1920 && height==1080 && !resolutionChanged &&
                !options.noBloom && !options.ptNoDenoise && options.debugView==0 &&
                options.benchmarkSeconds>=60 && measuredSeconds>=options.benchmarkSeconds-.1 &&
                percentile(wall,.95)<=33.3 && gpu.size()>=requiredTimingSamples &&
                trace.size()>=requiredTimingSamples && denoise.size()>=requiredTimingSamples;
        out << "{\n  \"renderer\": " << json(options.pathTracing?"path-traced":"raster")
            << ", \"frame_budget_ms\": " << (options.pathTracing?33.3:16.7)
            << ", \"pt_peak_bytes\": " << ptPeak << ",\n";

        out << "  \"project\": " << json(options.project.string()) << ",\n  \"width\": " << width
            << ", \"height\": " << height
            << ",\n  \"quality\": " << json(options.lowQuality ? "low" : "balanced")
            << ", \"gi_enabled\": " << (options.pathTracing?"false":"true") << ",\n  \"measured_frames\": " << frames.size()
            << ", \"unique_gpu_frames\": " << gpu.size() << ", \"unique_gi_frames\": " << gi.size()
            << ", \"bloom_enabled\": "
            << (!options.noBloom && (options.pathTracing || !options.lowQuality) ? "true" : "false")
            << ",\n  \"requested_seconds\": " << options.benchmarkSeconds
            << ", \"warmup_seconds\": " << options.warmupSeconds
            << ", \"measured_seconds\": " << measuredSeconds
            << ", \"resolution_changed\": " << (resolutionChanged ? "true" : "false")
            << ",\n  \"texture_peak_bytes\": " << texturePeak << ", \"gi_peak_bytes\": " << giPeak
            << ",\n  \"local_frame_gate_passed\": " << (gate ? "true" : "false")
            << ",\n  \"target_hardware_gate\": " << json(options.pathTracing ? "requires three runs on 8-core M3 and RTX 3060 12 GB (Windows and Linux runtime checks)" : "requires three runs on both M1 and GTX 1660") << ",\n";
        auto metric = [&](const char *name, const std::vector<double> &samples, bool last) {
            out << "  \"" << name << "\": ";
            if (samples.empty())
                out << "null";
            else
                out << "{\"p50\": " << percentile(samples, .5)
                    << ", \"p95\": " << percentile(samples, .95)
                    << ", \"p99\": " << percentile(samples, .99) << '}';
            out << (last ? "\n" : " ,\n");
        };
        metric("frame_ms", wall, false);
        metric("submit_ms", submit, false);
        metric("simulation_ms", simulation, false);
        metric("gpu_ms", gpu, false);
        metric("gi_ms", gi, false);
        metric("trace_ms", trace, false);
        metric("denoise_ms", denoise, false);
        metric("acceleration_ms", acceleration, true);
        out << "}\n";
        std::printf("Graphics benchmark %dx%d %s: %zu frames, frame p50 %.3f p95 %.3f p99 %.3f ms; "
                    "GPU p95 %.3f ms; GI p95 %.3f ms.\n",
                    width, height, options.lowQuality ? "Low" : "Balanced", frames.size(),
                    percentile(wall, .5), percentile(wall, .95), percentile(wall, .99),
                    percentile(gpu, .95), percentile(gi, .95));
        std::printf("Report: %s\n", path.string().c_str());
    }
};
} // namespace aspera
