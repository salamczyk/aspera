#include "aspera/engine.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
namespace {
volatile std::sig_atomic_t running = 1;
void stop(int) {
    running = 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::filesystem::path project = "games/demolition", save, load;
        uint16_t port = 24680;
        int ticks = 0;
        bool network = true, benchmark = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("Missing value for " + arg);
                return std::string(argv[i]);
            };
            if (arg == "--project")
                project = value();
            else if (arg == "--port") {
                auto text = value();
                size_t consumed = 0;
                int n = std::stoi(text, &consumed);
                if (consumed != text.size() || n < 1 || n > 65535)
                    throw std::runtime_error("Invalid port");
                port = uint16_t(n);
            } else if (arg == "--ticks") {
                auto text = value();
                size_t consumed = 0;
                ticks = std::stoi(text, &consumed);
                if (consumed != text.size() || ticks < 0)
                    throw std::runtime_error("--ticks must be a non-negative count");
            } else if (arg == "--save")
                save = value();
            else if (arg == "--load")
                load = value();
            else if (arg == "--no-network")
                network = false;
            else if (arg == "--benchmark")
                benchmark = true;
            else if (arg == "--help") {
                std::cout << "aspera_server [--project path] [--port 24680] [--ticks N] [--save "
                             "path] [--load path] [--no-network] [--benchmark]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + arg);
        }
        aspera::Engine engine;
        engine.loadProject(project);
        if (!load.empty()) {
            engine.world = aspera::loadWorld(load);
            engine.resetPhysics();
        }
        if (benchmark) {
            engine.world.objects.clear();
            for (int k = 0; k < 20; ++k) {
                aspera::VoxelGrid grid;
                for (int x = 0; x < 50; ++x)
                    for (int y = 0; y < 40; ++y)
                        for (int z = 0; z < 50; ++z)
                            grid.set({x, y, z}, 1);
                auto id = engine.world.spawn("Benchmark structure", std::move(grid),
                                             {{float(k % 5) * 6, 0, float(k / 5) * 6}, {}});
                auto &o = engine.world.objects.at(id);
                for (int x = 0; x < 50; ++x)
                    for (int z = 0; z < 50; ++z)
                        o.anchors.insert({x, 0, z});
            }
            for (int i = 0; i < 500; ++i) {
                aspera::VoxelGrid g;
                g.set({0, 0, 0}, 2);
                engine.world.spawn("Debris", std::move(g),
                                   {{float(i % 25) * .3f, 7, float(i / 25) * .3f}, {}}, true);
            }
            engine.resetPhysics();
            if (!ticks)
                ticks = 120;
        }
        aspera::Network session;
        if (network)
            session.host(port);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::cout << "Per Aspera server | " << engine.projectName() << " | "
                  << (network ? session.status() : "offline") << '\n';
        auto deadline = std::chrono::steady_clock::now();
        std::vector<double> samples;
        int frame = 0;
        while (running && (!ticks || frame < ticks)) {
            session.poll(engine);
            engine.step();
            session.broadcast(engine);
            session.poll(engine);
            samples.push_back(engine.stats.frameMs);
            ++frame;
            deadline += std::chrono::microseconds(16667);
            if (!benchmark)
                std::this_thread::sleep_until(deadline);
        }
        if (!save.empty())
            aspera::saveWorld(engine.world, save);
        std::sort(samples.begin(), samples.end());
        auto percentile = [&](float q) {
            return samples.empty()
                       ? 0
                       : samples[std::min(samples.size() - 1, size_t(q * samples.size()))];
        };
        std::cout << "ticks=" << frame << " objects=" << engine.world.objects.size()
                  << " p50_ms=" << percentile(.5) << " p95_ms=" << percentile(.95)
                  << " p99_ms=" << percentile(.99) << " nodes=" << engine.stats.structuralNodes
                  << " bodies=" << engine.stats.activeBodies
                  << " checksum=" << engine.world.checksum() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Server: " << e.what() << '\n';
        return 1;
    }
}
