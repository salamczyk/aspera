#include "render_benchmark.hpp"
#include <chrono>
#include <iostream>

using namespace aspera;

int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("aspera-render-gates-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        struct Case {
            const char *name;
            bool expected, noBloom{}, resized{}, staleGpu{}, staleGi{}, low{};
            double seconds{60}, frameMs{10};
        };
        for (const auto &test : {Case{"complete", true}, Case{"bloom-disabled", false, true},
                                 Case{"resize-and-restore", false, false, true},
                                 Case{"one-gpu-sample", false, false, false, true},
                                 Case{"one-gi-sample", false, false, false, false, true},
                                 Case{"low-quality", false, false, false, false, false, true},
                                 Case{"interrupted", false, false, false, false, false, false, 5},
                                 Case{"slow", false, false, false, false, false, false, 60, 20}}) {
            AppOptions options;
            options.renderBenchmark = directory / (std::string(test.name) + ".csv");
            options.noBloom = test.noBloom;
            options.lowQuality = test.low;
            RenderBenchmark benchmark(options, 1920, 1080);
            if (test.resized) {
                benchmark.resized(800, 600);
                benchmark.resized(1920, 1080);
            }
            for (uint32_t frame = 0; frame <= 6000; ++frame) {
                RenderTimings timings;
                timings.gpuFrame = test.staleGpu ? 1 : frame;
                timings.giFrame = test.staleGi ? 1 : frame;
                timings.gpuMs = 8;
                timings.giMs = 3;
                benchmark.record(test.seconds * frame / 6000, test.frameMs, 1, timings);
            }
            benchmark.write();
            auto report = options.renderBenchmark;
            report.replace_extension("json");
            std::ifstream input(report);
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            const bool passed =
                contents.find("\"local_frame_gate_passed\": true") != std::string::npos;
            if (passed != test.expected)
                throw std::runtime_error(std::string("Incorrect graphics qualification: ") +
                                         test.name);
        }
        for(int mode=0;mode<4;mode++){
            AppOptions options;options.pathTracing=true;options.renderBenchmark=directory/("pt-"+std::to_string(mode)+".csv");
            RenderBenchmark benchmark(options,1920,1080);
            for(uint32_t frame=0;frame<=1800;frame++){
                RenderTimings s;s.backend=mode==1?"raster":"path-traced (Metal)";s.gpuFrame=frame;s.gpuMs=25;s.traceMs=20;s.denoiseMs=mode==2?0:5;
                benchmark.record(double(frame)/30,mode==3?40:30,1,s);
            }
            benchmark.write();auto path=options.renderBenchmark;path.replace_extension("json");std::ifstream f(path);std::string text((std::istreambuf_iterator<char>(f)),{});
            const bool passed=text.find("\"local_frame_gate_passed\": true")!=std::string::npos;
            if(passed!=(mode==0))throw std::runtime_error("Incorrect path-tracing graphics qualification");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Graphics qualification regression checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
