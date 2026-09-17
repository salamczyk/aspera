#pragma once
#include "aspera/engine.hpp"
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace aspera {
// Shared player/editor rendering switches. Returns false for an unrelated flag.
inline bool parseRenderOption(AppOptions &options, std::string_view arg, int &i, int argc,
                              char **argv) {
    auto value = [&]() -> std::string_view {
        if (++i >= argc)
            throw std::runtime_error("Missing value for " + std::string(arg));
        return argv[i];
    };
    auto integer = [&](int low, int high) {
        auto text = value();
        int result{};
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
        if (error != std::errc{} || end != text.data() + text.size() || result < low ||
            result > high)
            throw std::runtime_error("Invalid value for " + std::string(arg));
        return result;
    };
    if (arg == "--quality") {
        auto text = value();
        if (text != "balanced" && text != "low")
            throw std::runtime_error("Quality must be balanced or low");
        options.lowQuality = text == "low";
    } else if (arg == "--no-bloom")
        options.noBloom = true;
    else if (arg == "--no-ui")
        options.noUi = true;
    else if (arg == "--fixed-step")
        options.fixedStep = true;
    else if (arg == "--render-smoke") {
        options.renderSmoke = true;
        options.fixedStep = true;
        if (!options.frameLimit)
            options.frameLimit = 96;
    } else if (arg == "--width") {
        options.width = integer(64, 8192);
        options.explicitSize = true;
    } else if (arg == "--height") {
        options.height = integer(64, 8192);
        options.explicitSize = true;
    } else if (arg == "--render-benchmark") {
        options.renderBenchmark = value();
        if (!options.explicitSize) {
            options.width = 1920;
            options.height = 1080;
        }
        options.explicitSize = true;
        options.noUi = true;
    } else if (arg == "--benchmark-seconds")
        options.benchmarkSeconds = float(integer(1, 3600));
    else if (arg == "--warmup-seconds")
        options.warmupSeconds = float(integer(0, 300));
    else if (arg == "--debug-view") {
        const auto text = value();
        constexpr std::string_view names[]{"final",    "material", "direct", "shadows",
                                           "indirect", "ao",       "voxels"};
        bool found = false;
        for (int index = 0; index < 7; ++index)
            if (text == names[index]) {
                options.debugView = index;
                found = true;
                break;
            }
        if (!found)
            throw std::runtime_error("Unknown debug view");
    } else if (arg == "--camera") {
        const std::string text(value());
        char trailing{};
        if (std::sscanf(text.c_str(), "%f,%f,%f,%f,%f%c", &options.cameraPosition.x,
                        &options.cameraPosition.y, &options.cameraPosition.z, &options.cameraYaw,
                        &options.cameraPitch, &trailing) != 5 ||
            !std::isfinite(options.cameraPosition.x) || !std::isfinite(options.cameraPosition.y) ||
            !std::isfinite(options.cameraPosition.z) || !std::isfinite(options.cameraYaw) ||
            !std::isfinite(options.cameraPitch) || std::abs(options.cameraPitch) > 1.55f)
            throw std::runtime_error("Camera requires finite x,y,z,yaw,pitch (radians)");
        options.cameraOverride = true;
    } else
        return false;
    return true;
}
inline constexpr const char *RenderOptionHelp =
    "  --quality balanced|low --no-bloom --no-ui --fixed-step --render-smoke\n"
    "  --width pixels --height pixels --camera x,y,z,yaw,pitch\n"
    "  --debug-view final|material|direct|shadows|indirect|ao|voxels\n"
    "  --render-benchmark report.csv --benchmark-seconds 60 --warmup-seconds 5\n";
} // namespace aspera
