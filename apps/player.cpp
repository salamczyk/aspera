#include "aspera/app_cli.hpp"
#include "aspera/engine.hpp"

#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

int main(int argc, char **argv) {
    aspera::AppOptions options;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            const auto value = [&]() -> std::string_view {
                return i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
            };
            if (aspera::parseRenderOption(options, arg, i, argc, argv))
                continue;
            if (arg == "--project")
                options.project = value();
            else if (arg == "--host")
                options.host = true;
            else if (arg == "--connect")
                options.connectAddress = value();
            else if (arg == "--port")
                aspera::parseInteger(options.port, arg, i, argc, argv);
            else if (arg == "--frames") {
                aspera::parseInteger(options.frameLimit, arg, i, argc, argv);
                if (options.frameLimit < 1)
                    throw std::runtime_error("--frames must be at least 1");
            } else if (arg == "--screenshot")
                options.screenshot = value();
            else if (arg == "--help") {
                std::puts("aspera_player [--project path] [--host] [--connect host] [--port n] "
                          "[--frames n] [--screenshot path]");
                std::puts(aspera::RenderOptionHelp);
                return 0;
            } else {
                std::fprintf(stderr, "Unknown option: %.*s\n", static_cast<int>(arg.size()),
                             arg.data());
                return 2;
            }
        }
        return aspera::runApp(options);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
