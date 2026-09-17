#include "aspera/voxel.hpp"
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
int main(int argc, char **argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "--new-project") {
            if (argc != 3)
                throw std::runtime_error("Usage: aspera_cooker --new-project directory");
            fs::path project = fs::absolute(argv[2]);
            if (fs::exists(project) && !fs::is_empty(project))
                throw std::runtime_error("New project directory must be empty");
            fs::create_directories(project / "assets");
            std::ofstream(project / "project.lua") << "return { name = 'New Voxel Game' }\n";
            std::ofstream(project / "scene.lua")
                << "engine.spawn_box('Foundation', -4, -0.1, -4, 80, 1, 80, 1, true)\n"
                   "engine.spawn_box('Timber block', -0.5, 0, -0.5, 10, 20, 10, 2, true)\n"
                   "engine.property('game.state', 0)\nengine.property('game.time', 0)\n";
            std::ofstream(project / "server.lua")
                << "function update(dt)\n  engine.property('game.time', "
                   "engine.property('game.time') + dt)\nend\n";
            std::cout << "Created project at " << project << '\n';
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "--package") {
            if (argc != 4)
                throw std::runtime_error(
                    "Usage: aspera_cooker --package project-directory output-directory");
            fs::path project = fs::canonical(argv[2]), out = fs::absolute(argv[3]);
            for (auto name : {"project.lua", "scene.lua", "server.lua"})
                if (!fs::is_regular_file(project / name))
                    throw std::runtime_error(std::string("Missing ") + name);
            if (fs::exists(out) && !fs::is_empty(out))
                throw std::runtime_error("Package output must be empty");
            auto binaries = fs::canonical(argv[0]).parent_path();
#ifdef _WIN32
            std::string extension = ".exe";
#else
            std::string extension;
#endif
            auto player = binaries / ("aspera_player" + extension);
            if (!fs::exists(player))
                throw std::runtime_error("Build the graphical player before packaging");
            auto shaders = binaries / "shaders";
            if (!fs::exists(shaders))
                shaders = binaries / "../shaders"; // Multi-configuration build/Release binaries.
            if (!fs::exists(shaders))
                shaders = binaries / "../share/aspera/shaders";
            if (!fs::exists(shaders))
                throw std::runtime_error("Compiled shaders missing");
            for (auto &entry : fs::recursive_directory_iterator(project))
                if (entry.is_symlink())
                    throw std::runtime_error("Package assets must not be symbolic links");
            fs::create_directories(out);
            fs::copy_file(player, out / player.filename());
            auto server = binaries / ("aspera_server" + extension);
            if (fs::exists(server))
                fs::copy_file(server, out / server.filename());
            fs::copy(project, out / "game", fs::copy_options::recursive);
            fs::copy(shaders, out / "shaders", fs::copy_options::recursive);
            auto notices = binaries / "licenses";
            if (!fs::exists(notices))
                notices = binaries / "../licenses";
            if (!fs::exists(notices))
                notices = binaries / "../share/aspera/licenses";
            if (fs::exists(notices))
                fs::copy(notices, out / "licenses", fs::copy_options::recursive);
            std::ofstream(out / "Play.bat")
                << "@echo off\r\ncd /d \"%~dp0\"\r\naspera_player.exe --project game %*\r\n";
            std::ofstream(out / "Play.sh") << "#!/bin/sh\ncd -- \"$(dirname -- \"$0\")\" || exit "
                                              "1\nexec ./aspera_player --project game \"$@\"\n";
            fs::permissions(out / "Play.sh",
                            fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add);
            std::cout << "Packaged game at " << out << " (for this operating system)\n";
            return 0;
        }
        if (argc < 3) {
            std::cout << "Usage: aspera_cooker model.vox output.avox [voxel-size]\n       "
                         "aspera_cooker --package project-directory output-directory\n"
                         "       aspera_cooker --new-project directory\n";
            return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 1;
        }
        float size = argc > 3 ? std::stof(argv[3]) : .1f;
        auto scene = aspera::importVox(argv[1], size);
        aspera::writeAsset(scene, argv[2]);
        size_t cells = 0;
        for (auto &o : scene.objects)
            cells += o.grid.count();
        std::cout << "Cooked " << scene.objects.size() << " objects, " << cells << " voxels\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
