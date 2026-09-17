#include "aspera/app_cli.hpp"
#include "aspera/engine.hpp"

#include <charconv>
#include <cstdio>
#include <string_view>

int main(int argc, char **argv) {
    aspera::AppOptions options;
    options.editor = true;
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
            else if (arg == "--stress-preview")
                options.stressPreview = true;
            else if (arg == "--connect")
                options.connectAddress = value();
            else if (arg == "--port") {
                const auto text = value();
                std::from_chars(text.data(), text.data() + text.size(), options.port);
            } else if (arg == "--frames") {
                const auto text = value();
                std::from_chars(text.data(), text.data() + text.size(), options.frameLimit);
            } else if (arg == "--screenshot")
                options.screenshot = value();
            else if (arg == "--help") {
                std::puts("aspera_editor [--project path] [--host] [--connect host] [--port n] "
                          "[--frames n] [--screenshot path] [--stress-preview]");
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
