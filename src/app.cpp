#include "aspera/camera.hpp"
#include "aspera/engine.hpp"
#include "aspera/undo.hpp"
#include "render.hpp"
#include "render_benchmark.hpp"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef ASPERA_SHADER_DIR
#define ASPERA_SHADER_DIR "assets/shaders"
#endif

namespace aspera {
namespace {
std::string lower(std::string text) {
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

struct Camera {
    Vec3 position{5.f, 4.f, -7.f};
    // Starts above the south-west work-pad edge, aimed at the origin used by
    // both bundled Lua scenes.
    float yaw{-0.62f};
    float pitch{-0.42f};
    bool topDown{};

    Vec3 forward() const {
        return normalized(
            {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)});
    }
    Vec3 right() const {
        return normalized(cross({0.f, 1.f, 0.f}, forward()));
    }
    Vec3 eye() const {
        return topDown ? Vec3{position.x, std::max(position.y, 8.f), position.z} : position;
    }
    Vec3 rayDirection() const {
        return topDown ? normalized(Vec3{-5.f, -8.f, 7.f}) : forward();
    }
    Vec3 aimDirection(float x, float y) const {
        const auto size = ImGui::GetIO().DisplaySize;
        return perspectivePickRay(rayDirection(), x, y, size.x, size.y);
    }
    void view(float *out) const {
        if (topDown) {
            const Vec3 eye{position.x, std::max(position.y, 8.f), position.z};
            const auto target = eye + rayDirection();
            bx::mtxLookAt(out, {eye.x, eye.y, eye.z}, {target.x, target.y, target.z});
            return;
        }
        const Vec3 look = position + forward();
        bx::mtxLookAt(out, {position.x, position.y, position.z}, {look.x, look.y, look.z});
    }
};

enum class Tool { Add, Remove, Paint, Anchor, Prefab };
struct Session {
    EntityId selected{};
    EntityId jointFirst{};
    MaterialId material{1};
    Tool tool{Tool::Add};
    bool playing{};
    bool recoverRequested{};
    bool showStats{true};
    bool showEvents{true};
    bool stress{};
    bool showLighting{};
    LightId selectedLight{};
    UndoHistory history{};
    bool showLandscape{};
    LandscapeSettings landscapeSettings;
    uint64_t landscapeCommand{};
    double landscapeStarted{};
    std::string landscapeMessage;
    std::vector<uint8_t> landscapeUndo;
    std::vector<uint8_t> playSnapshot;
};

void snapshot(Engine &engine, Session &state) {
    state.history.record(encodeWorld(engine.world));
}
void undo(Engine &engine, Session &state) {
    if (!state.history.canUndo())
        return;
    auto bytes = state.history.undo(encodeWorld(engine.world));
    engine.world = decodeWorld(*bytes);
    engine.resetPhysics();
}
void redo(Engine &engine, Session &state) {
    if (!state.history.canRedo())
        return;
    auto bytes = state.history.redo(encodeWorld(engine.world));
    engine.world = decodeWorld(*bytes);
    engine.resetPhysics();
}
void landscapeCompletion(Engine &engine, Session &state) {
    if (!state.landscapeCommand)
        return;
    for (const auto &event : engine.events) {
        if (event.command != state.landscapeCommand ||
            (event.type != "committed" && event.type != "rejected"))
            continue;
        if (event.type == "committed") {
            state.history.record(std::move(state.landscapeUndo));
            state.selected = event.entity;
        }
        state.landscapeUndo.clear();
        state.landscapeCommand = 0;
        state.landscapeMessage = event.message;
        break;
    }
}
void landscapeUi(Engine &engine, Network &network, Session &state) {
    if (!state.showLandscape)
        return;
    ImGui::SetNextWindowSize({390, 540}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LANDSCAPE", &state.showLandscape)) {
        ImGui::End();
        return;
    }
    auto &settings = state.landscapeSettings;
    ImGui::TextWrapped("Seeded voxel terrain with caves and anchored ground. Bounds and noise "
                       "scales are in cells.");
    ImGui::BeginDisabled(network.isClient() || state.playing || state.landscapeCommand != 0);
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &settings.seed);
    auto bound = [&](const char *label, IVec3 &p) {
        int values[3]{p.x, p.y, p.z};
        if (ImGui::InputInt3(label, values))
            p = {values[0], values[1], values[2]};
    };
    bound("Minimum", settings.minimum);
    bound("Maximum (exclusive)", settings.maximum);
    ImGui::InputFloat("Surface height", &settings.surfaceHeight);
    ImGui::InputFloat("Hill amplitude", &settings.hillAmplitude);
    ImGui::InputFloat("Feature scale", &settings.featureScale);
    ImGui::SliderFloat("Cave threshold", &settings.caveThreshold, 0, 1);
    ImGui::InputFloat("Overhang strength", &settings.overhangStrength);
    ImGui::InputInt("Grass depth", &settings.grassDepth);
    ImGui::InputInt("Soil depth", &settings.soilDepth);
    ImGui::InputInt("Foundation depth", &settings.bottomDepth);
    auto material = [&](const char *label, MaterialId &id) {
        const char *name = id < engine.world.materials.size()
                               ? engine.world.materials[id].name.c_str()
                               : "Select material";
        if (ImGui::BeginCombo(label, name)) {
            for (size_t i = 1; i < engine.world.materials.size(); ++i) {
                ImGui::PushID(int(i));
                if (ImGui::Selectable(engine.world.materials[i].name.c_str(), id == i))
                    id = MaterialId(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    };
    material("Grass", settings.grassMaterial);
    material("Soil", settings.soilMaterial);
    material("Rock", settings.rockMaterial);
    ImGui::BeginDisabled(engine.stats.pendingCommands != 0);
    if (ImGui::Button("Generate new landscape")) {
        try {
            validateLandscapeSettings(settings);
            state.landscapeUndo = encodeWorld(engine.world);
            state.landscapeStarted = ImGui::GetTime();
            state.landscapeCommand = engine.requestLandscape(settings);
            state.landscapeMessage.clear();
        } catch (const std::exception &ex) {
            state.landscapeMessage = ex.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (state.landscapeCommand)
        ImGui::TextWrapped("Generating and validating terrain... %.1f s",
                           ImGui::GetTime() - state.landscapeStarted);
    else if (!state.landscapeMessage.empty())
        ImGui::TextWrapped("%s", state.landscapeMessage.c_str());
    if (state.playing)
        ImGui::TextDisabled("Stop Play to generate terrain.");
    ImGui::Text("Undo history: %.1f / 256 MiB", double(state.history.bytes()) / (1024 * 1024));
    ImGui::End();
}

void submit(Engine &engine, Network &network, Command command) {
    if (network.isClient())
        network.sendCommand(command);
    else
        engine.submit(command);
}

void actionAtPointer(Engine &engine, Network &network, Session &state, const Camera &camera,
                     float x, float y, bool remove) {
    const auto hit = raycastWorld(engine.world, camera.eye(), camera.aimDirection(x, y), 100.f);
    if (!hit)
        return;
    Command command;
    command.entity = hit->entity;
    command.expectedRevision = engine.world.objects[hit->entity].revision;
    command.cell = hit->hit.cell;
    command.material = state.material;
    command.point = hit->worldPoint;
    switch (state.tool) {
    case Tool::Add:
        command.type = CommandType::SetVoxel;
        command.cell = command.cell + hit->hit.normal;
        break;
    case Tool::Remove:
        command.type = CommandType::SetVoxel;
        command.material = 0;
        break;
    case Tool::Paint:
        command.type = CommandType::Paint;
        break;
    case Tool::Anchor:
        command.type = CommandType::Anchor;
        break;
    case Tool::Prefab:
        command.type = CommandType::SpawnPrefab;
        command.cell = command.cell + hit->hit.normal;
        command.vector = {float(hit->hit.normal.x), float(hit->hit.normal.y),
                          float(hit->hit.normal.z)};
        break;
    }
    if (remove) {
        command.type = CommandType::SetVoxel;
        command.material = 0;
    }
    submit(engine, network, command);
}

void lightingUi(Engine &engine, Network &network, Session &state, Renderer &renderer,
                const Camera &camera) {
    if (!state.showLighting)
        return;
    ImGui::SetNextWindowSize({365, 600}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LIGHTING", &state.showLighting)) {
        ImGui::End();
        return;
    }
    int quality = renderer.quality() == RenderQuality::Low ? 1 : 0;
    ImGui::BeginDisabled(renderer.pathTraced());
    if (ImGui::Combo("Quality", &quality, "Balanced\0Low\0"))
        renderer.setQuality(quality ? RenderQuality::Low : RenderQuality::Balanced);
    ImGui::EndDisabled();
    int debug = int(renderer.debug());
    if (ImGui::Combo("View", &debug,
                     renderer.pathTraced() ? "Final\0Material\0Normal\0Diffuse\0Specular\0Depth\0History\0" :
                     "Final\0Material\0Direct light\0Sun shadows\0Indirect light\0Contact AO\0Voxel visibility\0"))
        renderer.setDebug(RenderDebug(debug));
    const auto stats = renderer.timings();
    ImGui::Text("Renderer: %s (restart to change)", stats.backend.c_str());
    if (!stats.fallbackReason.empty()) ImGui::TextWrapped("%s", stats.fallbackReason.c_str());
    if (stats.ptBounces) {
        ImGui::Text("%u spp | %u bounces | accumulated %u",stats.ptSamples,stats.ptBounces,stats.accumulatedSamples);
        ImGui::Text("Trace %.2f | denoise %.2f | AS %.2f ms",stats.traceMs,stats.denoiseMs,stats.accelerationMs);
        ImGui::Text("Path tracing %.1f MiB",stats.ptBytes/1048576.);
    }
    ImGui::Text("GPU %.2f ms | GI %.2f ms", stats.gpuMs, stats.giMs);
    ImGui::Text("Textures %.1f MiB | GI %.1f MiB", stats.textureBytes / 1048576.,
                stats.giBytes / 1048576.);
    ImGui::Text("GI bricks updated: %u", stats.pendingGi);
    if (ImGui::TreeNode("Pass timings")) {
        for (const auto &pass : stats.passes)
            ImGui::Text("%s: %.2f ms GPU", pass.name.c_str(), pass.gpuMs);
        ImGui::TreePop();
    }
    if (network.isClient())
        ImGui::TextWrapped("The host authors scene lighting. Quality and debug views are local.");
    ImGui::BeginDisabled(network.isClient());
    bool changed = false, activated = false;
    auto drag = [&](const char *label, float *value, float speed, float min, float max) {
        changed |= ImGui::DragFloat(label, value, speed, min, max);
        activated |= ImGui::IsItemActivated();
    };
    auto vector = [&](const char *label, Vec3 &value, float speed) {
        changed |= ImGui::DragFloat3(label, &value.x, speed);
        activated |= ImGui::IsItemActivated();
    };
    auto color = [&](const char *label, Vec3 &value) {
        changed |= ImGui::ColorEdit3(label, &value.x);
        activated |= ImGui::IsItemActivated();
    };
    if (ImGui::CollapsingHeader("Sun and sky", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto environment = engine.world.environment;
        vector("Sun direction", environment.sunDirection, .01f);
        color("Sun colour", environment.sunColor);
        drag("Sun lux", &environment.sunIntensity, 100, 0, 150000);
        color("Sky colour", environment.skyColor);
        drag("Sky lux", &environment.skyIntensity, 100, 0, 100000);
        drag("Exposure EV100", &environment.exposure, .05f, -10, 20);
        drag("Bloom", &environment.bloomStrength, .005f, 0, 1);
        if (activated)
            snapshot(engine, state);
        if (changed) {
            try {
                engine.setEnvironment(environment);
            } catch (const std::exception &error) {
                engine.emit("lighting_error", 0, error.what());
            }
        }
        if (ImGui::Button("Reset daylight")) {
            snapshot(engine, state);
            engine.setEnvironment({});
        }
    }
    ImGui::SeparatorText("LOCAL LIGHTS");
    ImGui::BeginDisabled(engine.world.lights.size() >= 64);
    bool point = ImGui::Button("Add point");
    ImGui::SameLine();
    bool spot = ImGui::Button("Add spot");
    if (point || spot) {
        Light light;
        light.position = camera.eye() + camera.rayDirection() * 2.f;
        light.direction = camera.rayDirection();
        light.type = spot ? LightType::Spot : LightType::Point;
        snapshot(engine, state);
        state.selectedLight = engine.addLight(light);
    }
    ImGui::EndDisabled();
    for (const auto &[id, light] : engine.world.lights) {
        const std::string title =
            std::string(light.type == LightType::Point ? "Point " : "Spot ") + std::to_string(id);
        if (ImGui::Selectable(title.c_str(), state.selectedLight == id))
            state.selectedLight = id;
    }
    auto it = engine.world.lights.find(state.selectedLight);
    if (it != engine.world.lights.end()) {
        auto light = it->second;
        changed = activated = false;
        vector("Position", light.position, .05f);
        color("Colour", light.color);
        drag("Intensity lumens", &light.intensity, 20, 0, 1000000);
        drag("Range metres", &light.range, .1f, .1f, 1000);
        if (light.type == LightType::Spot) {
            vector("Direction", light.direction, .01f);
            drag("Inner cone degrees", &light.innerAngle, .25f, 0, light.outerAngle - .1f);
            drag("Outer cone degrees", &light.outerAngle, .25f, light.innerAngle + .1f, 89);
        }
        changed |= ImGui::Checkbox("Cast shadows", &light.shadows);
        activated |= ImGui::IsItemActivated();
        int priority = light.shadowPriority;
        changed |= ImGui::DragInt("Shadow priority", &priority, 1, -100, 100);
        activated |= ImGui::IsItemActivated();
        light.shadowPriority = priority;
        if (activated)
            snapshot(engine, state);
        if (changed) {
            try {
                engine.updateLight(light.id, light);
            } catch (const std::exception &error) {
                engine.emit("lighting_error", 0, error.what());
            }
        }
        if (ImGui::Button("Place in front of camera")) {
            snapshot(engine, state);
            light.position = camera.eye() + camera.rayDirection() * 2.f;
            light.direction = camera.rayDirection();
            engine.updateLight(light.id, light);
        }
        if (ImGui::Button("Delete light")) {
            snapshot(engine, state);
            engine.removeLight(light.id);
            state.selectedLight = 0;
        }
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void editorUi(Engine &engine, Network &network, const AppOptions &options, Session &state,
              Renderer &renderer, const Camera &camera) {
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_Always);
    // The command bar follows the current logical window width, including resize/DPI changes.
    ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x - 24, 84}, ImGuiCond_Always);
    ImGui::Begin("ASPERA // COMMAND", nullptr,
                 ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                try {
                    saveWorld(engine.world, options.project / "scene.aspera");
                    engine.emit("scene", 0, "Scene saved");
                } catch (const std::exception &error) {
                    engine.emit("scene_error", 0, error.what());
                }
            }
            ImGui::BeginDisabled(network.isClient());
            if (ImGui::MenuItem("Load") &&
                std::filesystem::exists(options.project / "scene.aspera")) {
                try {
                    snapshot(engine, state);
                    engine.world = loadWorld(options.project / "scene.aspera");
                    renderer.invalidateMeshes();
                    engine.resetPhysics();
                } catch (const std::exception &error) {
                    engine.emit("scene_error", 0, error.what());
                }
            }
            if (ImGui::MenuItem("Reload Lua")) {
                try {
                    engine.reloadScripts();
                } catch (const std::exception &error) {
                    engine.emit("script_error", 0, error.what());
                }
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Stats", nullptr, &state.showStats);
            ImGui::MenuItem("Events", nullptr, &state.showEvents);
            ImGui::MenuItem("Lighting", nullptr, &state.showLighting);
            ImGui::MenuItem("Landscape", nullptr, &state.showLandscape);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::BeginDisabled(network.isClient());
    if (ImGui::Button(state.playing ? "STOP [F6]" : "PLAY [F5]")) {
        if (state.playing) {
            engine.world = decodeWorld(state.playSnapshot);
            renderer.invalidateMeshes();
            engine.resetPhysics();
            state.playing = false;
        } else {
            state.playSnapshot = encodeWorld(engine.world);
            state.playing = true;
        }
    }
    ImGui::SameLine();
    if ((ImGui::Button("UNDO") || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) &&
        !network.isClient()) {
        undo(engine, state);
        renderer.invalidateMeshes();
    }
    ImGui::SameLine();
    if ((ImGui::Button("REDO") || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) &&
        !network.isClient()) {
        redo(engine, state);
        renderer.invalidateMeshes();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("LIGHTING"))
        state.showLighting = !state.showLighting;
    ImGui::SameLine();
    if (ImGui::Button("LANDSCAPE"))
        state.showLandscape = !state.showLandscape;
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", network.status().c_str());
    ImGui::End();

    ImGui::SetNextWindowPos({12, 108}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({240, 260}, ImGuiCond_FirstUseEver);
    ImGui::Begin("HIERARCHY");
    for (const auto &[id, object] : engine.world.objects) {
        const bool selected = id == state.selected;
        if (ImGui::Selectable((object.name + "##" + std::to_string(id)).c_str(), selected))
            state.selected = id;
    }
    ImGui::Separator();
    const bool fixed = ImGui::Button("Fixed Joint");
    ImGui::SameLine();
    const bool hinged = ImGui::Button("Hinge Joint");
    if (fixed || hinged) {
        const bool hinge = hinged;
        if (!state.jointFirst)
            state.jointFirst = state.selected;
        else if (state.selected && state.selected != state.jointFirst) {
            snapshot(engine, state);
            if (engine.world.objects.contains(state.jointFirst)) {
                uint64_t next = 1;
                for (auto &joint : engine.world.joints)
                    next = std::max(next, joint.id + 1);
                auto pivot = (engine.world.objects.at(state.jointFirst).transform.position +
                              engine.world.objects.at(state.selected).transform.position) *
                             .5f;
                engine.world.joints.push_back(
                    {next, state.jointFirst, state.selected, pivot, hinge});
                ++engine.world.revision;
            }
            state.jointFirst = 0;
        }
    }
    ImGui::TextDisabled(state.jointFirst ? "Select the second object"
                                         : "Choose two objects to join");
    ImGui::End();

    ImGui::SetNextWindowPos({12, 380}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({240, 480}, ImGuiCond_FirstUseEver);
    ImGui::Begin("INSPECTOR");
    if (auto it = engine.world.objects.find(state.selected); it != engine.world.objects.end()) {
        Object &object = it->second;
        ImGui::Text("#%llu  %s", static_cast<unsigned long long>(object.id),
                    object.dynamic ? "DYNAMIC" : "STATIC");
        float p[3]{object.transform.position.x, object.transform.position.y,
                   object.transform.position.z};
        ImGui::BeginDisabled(network.isClient());
        const bool moved = ImGui::DragFloat3("Position", p, 0.05f);
        if (ImGui::IsItemActivated())
            snapshot(engine, state);
        if (moved) {
            object.transform.position = {p[0], p[1], p[2]};
            ++object.revision;
            ++engine.world.revision;
            engine.resetPhysics();
        }
        ImGui::EndDisabled();
        ImGui::Text("Voxels: %zu", object.grid.count());
        static float launchSpeed = 10;
        ImGui::SetNextItemWidth(104);
        ImGui::SliderFloat("Launch m/s", &launchSpeed, 0, 50);
        ImGui::BeginDisabled(!state.playing);
        bool drop = ImGui::Button("Release / Drop");
        ImGui::SameLine();
        bool launch = ImGui::Button("Launch");
        if (drop || launch) {
            Command c;
            c.type = CommandType::ReleaseBody;
            c.entity = object.id;
            c.expectedRevision = object.revision;
            c.vector = launch ? camera.rayDirection() * launchSpeed : Vec3{};
            submit(engine, network, c);
        }
        ImGui::EndDisabled();
        if (std::any_of(engine.world.joints.begin(), engine.world.joints.end(),
                        [&](auto &j) { return j.a == object.id || j.b == object.id; }))
            ImGui::TextWrapped("Remove attached joints before Drop or Launch.");
        ImGui::Checkbox("Stress overlay", &state.stress);
        if (state.stress) {
            if (object.stressRegions.empty())
                ImGui::TextWrapped(
                    "No stress analysis yet. Run Play on the host to calculate loads.");
            else {
                ImGui::Text("Peak load: %.1f%%", object.stress * 100.f);
                ImGui::TextWrapped(
                    "Region outlines: green 0%%, yellow 50%%, red 100%%+ capacity. Internal "
                    "regions are visible through surfaces. Display capped at 4096 regions.");
                if (!state.playing)
                    ImGui::TextWrapped(
                        "Analysis paused; values are from the last simulation tick.");
            }
        }
    } else
        ImGui::TextDisabled("Select an object");
    if (!engine.world.materials.empty()) {
        ImGui::SeparatorText("MATERIAL");
        ImGui::BeginDisabled(network.isClient());
        state.material = std::min<MaterialId>(
            state.material, static_cast<MaterialId>(engine.world.materials.size() - 1));
        Material &material = engine.world.materials[state.material];
        ImGui::Text("%s [%u]", material.name.c_str(), state.material);
        Material edited = material;
        bool changed = false, started = false;
        auto slider = [&](const char *label, float &value, float speed, float min, float max) {
            ImGui::SetNextItemWidth(104);
            changed |= ImGui::DragFloat(label, &value, speed, min, max);
            started |= ImGui::IsItemActivated();
        };
        float colour[4]{float((material.rgba >> 24) & 255) / 255,
                        float((material.rgba >> 16) & 255) / 255,
                        float((material.rgba >> 8) & 255) / 255, 1};
        changed |= ImGui::ColorEdit3("Colour", colour);
        started |= ImGui::IsItemActivated();
        edited.rgba = (uint32_t(colour[0] * 255) << 24) | (uint32_t(colour[1] * 255) << 16) |
                      (uint32_t(colour[2] * 255) << 8) | 255;
        slider("Roughness", edited.roughness, .01f, .04f, 1);
        slider("Metallic", edited.metallic, .01f, 0, 1);
        changed |= ImGui::ColorEdit3("Emission", &edited.emissive.x);
        started |= ImGui::IsItemActivated();
        slider("Emission nits", edited.emissionIntensity, 10, 0, 60000);
        slider("Density", edited.density, 10, 1, 30000);
        slider("Resistance", edited.resistance, .02f, .01f, 100);
        slider("Friction", edited.friction, .01f, 0, 1);
        float toughness = edited.impactToughness / 1000.f;
        slider("Impact kJ/m3", toughness, 10, .001f, 1000000);
        edited.impactToughness = toughness * 1000.f;
        for (auto entry : std::array<std::pair<const char *, float *>, 4>{
                 {{"Tension MPa", &edited.tension},
                  {"Compression MPa", &edited.compression},
                  {"Shear MPa", &edited.shear},
                  {"Bending MPa", &edited.bending}}}) {
            float mpa = *entry.second / 1e6f;
            slider(entry.first, mpa, .05f, .001f, 1000);
            *entry.second = mpa * 1e6f;
        }
        if (started)
            snapshot(engine, state);
        if (changed) {
            try {
                engine.setMaterial(state.material, edited);
            } catch (const std::exception &error) {
                engine.emit("lighting_error", 0, error.what());
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::End();

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 12, 108}, ImGuiCond_Always, {1, 0});
    ImGui::SetNextWindowSize({308, 290}, ImGuiCond_FirstUseEver);
    ImGui::Begin("VOXEL TOOLS", nullptr, ImGuiWindowFlags_NoMove);
    const std::array<std::pair<const char *, Tool>, 5> tools{{{"Add", Tool::Add},
                                                              {"Remove", Tool::Remove},
                                                              {"Paint", Tool::Paint},
                                                              {"Anchor", Tool::Anchor},
                                                              {"Prefab", Tool::Prefab}}};
    int toolIndex = 0;
    for (const auto &[name, tool] : tools) {
        if (ImGui::RadioButton(name, state.tool == tool))
            state.tool = tool;
        if (++toolIndex % 3)
            ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::BeginCombo("Material", engine.world.materials.empty()
                                          ? "none"
                                          : engine.world.materials[state.material].name.c_str())) {
        for (MaterialId i = 0; i < engine.world.materials.size(); ++i)
            if (ImGui::Selectable(engine.world.materials[i].name.c_str(), state.material == i))
                state.material = i;
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("Click any voxel to edit, including during Play. Hold RMB to look.");
    static char assetPath[512]{};
    ImGui::SeparatorText("ASSET IMPORT");
    ImGui::InputText(".vox / .avox", assetPath, sizeof(assetPath));
    if (ImGui::Button("Import") && assetPath[0]) {
        try {
            snapshot(engine, state);
            const auto imported = engine.importAsset(assetPath);
            if (!imported.empty())
                state.selected = imported.front();
            engine.resetPhysics();
        } catch (const std::exception &error) {
            engine.emit("import_error", 0, error.what());
        }
    }
    ImGui::End();
}

bool playerProperty(const std::string &key) {
    return key != "game.state" && key != "_content_hash" && key.find("_id") == std::string::npos &&
           key.find("_initial") == std::string::npos &&
           key.find("initialized") == std::string::npos &&
           key.find("attack_at") == std::string::npos;
}

void playerValue(const std::string &key, double value) {
    if (key.find("progress") != std::string::npos || key.find("integrity") != std::string::npos)
        ImGui::Text("%s: %.0f%%", key.c_str(), value * 100.0);
    else
        ImGui::Text("%s: %.1f", key.c_str(), value);
}

bool playerUi(Engine &engine, Network &network, Session &state, const Camera &camera,
              bool characterMode) {
    ImGui::SetNextWindowBgAlpha(.82f);
    ImGui::SetNextWindowPos({16, 16}, ImGuiCond_Always);
    ImGui::Begin("HUD", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove);
    ImGui::TextColored({0.13f, .93f, .76f, 1.f}, "%s", engine.projectName().c_str());
    const auto stateProperty = engine.world.properties.find("game.state");
    const int gameState = stateProperty == engine.world.properties.end()
                              ? 0
                              : static_cast<int>(stateProperty->second);
    if (gameState == 1)
        ImGui::TextColored({0.18f, .95f, .57f, 1.f}, "OBJECTIVE COMPLETE");
    else if (gameState == 2)
        ImGui::TextColored({1.f, .30f, .18f, 1.f}, "OBJECTIVE FAILED");
    else
        ImGui::Text("Objective active");
    if (characterMode)
        ImGui::Text("Players: %zu / 4", engine.players().size());
    else
        ImGui::Text("Structures: %u  |  Peers: %zu", engine.stats.activeBodies, network.peers());
    ImGui::TextDisabled("Session: %s", network.status().c_str());
    if (characterMode) {
        ImGui::TextWrapped("Collapse the marked works while protecting the amber core.");
        ImGui::TextDisabled(
            "WASD walk · SPACE jump · LMB damage · RMB build · R recover · ESC cursor");
        PlayerId localId = network.isClient() ? network.localPlayer() : 1;
        ImGui::TextDisabled("Player colors: 1 blue · 2 amber · 3 green · 4 brown");
        if (localId && engine.players().contains(localId) && ImGui::Button("Recover to spawn"))
            state.recoverRequested = true;
        for (const auto &[id, player] : engine.players()) {
            ImGui::Text("Player %u%s%s", id, id == localId ? " (you)" : "",
                        player.spawnBlocked ? " — spawn blocked" : "");
        }
        if (auto hit = raycastWorld(engine.world, camera.eye(), camera.rayDirection(), 4))
            ImGui::Text("Tool target: %.1f m", length(hit->worldPoint - camera.eye()));
        else
            ImGui::TextDisabled("No surface within 4 m");
        for (auto event = engine.events.rbegin(); event != engine.events.rend(); ++event) {
            if (engine.world.tick >= event->tick && engine.world.tick - event->tick > 120)
                break;
            if (event->type == "tool_rejected" || event->type == "rejected") {
                ImGui::TextWrapped("%s", event->message.c_str());
                break;
            }
        }
    } else
        ImGui::TextDisabled("WASD move · LMB damage · RMB build · MMB look · TAB camera");
    ImGui::Separator();
    if (characterMode) {
        const auto &properties = engine.world.properties;
        if (auto value = properties.find("game.progress"); value != properties.end())
            ImGui::Text("Works removed: %.0f%% / 68%%", value->second * 100);
        if (auto value = properties.find("demolition.core_integrity"); value != properties.end())
            ImGui::Text("Core integrity: %.0f%% (minimum 80%%)", value->second * 100);
        if (auto value = properties.find("game.time"); value != properties.end())
            ImGui::Text("Elapsed: %.1f s", value->second);
    } else
        for (const auto &[key, value] : engine.world.properties)
            if (playerProperty(key))
                playerValue(key, value);
    const bool restart = gameState != 0 && !network.isClient() && ImGui::Button("Restart project");
    ImGui::End();
    float aimX = 0, aimY = 0;
    SDL_GetMouseState(&aimX, &aimY);
    if (characterMode) {
        aimX = ImGui::GetIO().DisplaySize.x * .5f;
        aimY = ImGui::GetIO().DisplaySize.y * .5f;
    }
    ImGui::SetNextWindowPos({aimX - 6.f, aimY - 9.f}, ImGuiCond_Always);
    ImGui::Begin("##crosshair", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
    ImGui::TextColored({1.f, .68f, .20f, 1.f}, "+");
    ImGui::End();
    (void)state;
    (void)camera;
    return restart;
}

void style() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 3;
    s.FrameRounding = 2;
    s.GrabRounding = 2;
    s.WindowPadding = {10, 9};
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = {.045f, .07f, .075f, .96f};
    c[ImGuiCol_TitleBg] = {.07f, .13f, .14f, 1};
    c[ImGuiCol_TitleBgActive] = {.08f, .23f, .23f, 1};
    c[ImGuiCol_Button] = {.11f, .24f, .24f, 1};
    c[ImGuiCol_ButtonHovered] = {.15f, .39f, .37f, 1};
    c[ImGuiCol_ButtonActive] = {.05f, .55f, .45f, 1};
    c[ImGuiCol_Header] = {.16f, .30f, .28f, 1};
    c[ImGuiCol_HeaderHovered] = {.20f, .45f, .40f, 1};
    c[ImGuiCol_CheckMark] = {1.f, .62f, .18f, 1};
    c[ImGuiCol_SliderGrab] = {1.f, .57f, .14f, 1};
    c[ImGuiCol_Separator] = {.18f, .38f, .36f, .9f};
}

} // namespace

int runApp(const AppOptions &options) {
    if (options.ptCaptureSamples && (!options.pathTracing || options.screenshot.empty() ||
        options.host || !options.connectAddress.empty() || !options.renderBenchmark.empty() || options.renderSmoke))
        throw std::runtime_error("Path-tracing reference capture requires --renderer path-traced, --screenshot, and an offline non-benchmark session");
    if (options.renderSmoke &&
        (options.host || !options.connectAddress.empty() || !options.renderBenchmark.empty()))
        throw std::runtime_error("Render smoke testing requires an offline, non-benchmark session");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }
    int windowWidth = options.width, windowHeight = options.height;
    if (!options.renderBenchmark.empty()) {
        if (auto mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay()); mode && mode->pixel_density > 0) {
            windowWidth = std::max(1, int(std::lround(options.width / mode->pixel_density)));
            windowHeight = std::max(1, int(std::lround(options.height / mode->pixel_density)));
        }
    }
    SDL_Window *window = SDL_CreateWindow(
        options.editor ? "Per Aspera — Editor" : "Per Aspera", windowWidth, windowHeight,
        SDL_WINDOW_RESIZABLE | ((options.explicitSize && options.renderBenchmark.empty()) ? 0 : SDL_WINDOW_HIGH_PIXEL_DENSITY)
#if !defined(__APPLE__) && defined(ASPERA_HAS_VULKAN_PT)
        | (options.pathTracing ? SDL_WINDOW_VULKAN : 0)
#endif
        );
#if !defined(__APPLE__) && defined(ASPERA_HAS_VULKAN_PT)
    if (!window && options.pathTracing) {
        SDL_Log("Vulkan window unavailable: %s; trying raster window", SDL_GetError());
        window = SDL_CreateWindow(options.editor ? "Per Aspera — Editor" : "Per Aspera",
            options.width, options.height, SDL_WINDOW_RESIZABLE |
            ((options.explicitSize && options.renderBenchmark.empty()) ? 0 : SDL_WINDOW_HIGH_PIXEL_DENSITY));
    }
#endif
    if (!window) {
        SDL_Log("Window failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Benchmark sizes are physical output pixels. Retina logical window dimensions
    // must fit the display without letting the window manager clamp the target.
    if (!options.renderBenchmark.empty()) {
        float density = SDL_GetWindowPixelDensity(window);
        if (density > 0 && (windowWidth != int(std::lround(options.width / density)) ||
                             windowHeight != int(std::lround(options.height / density))))
            SDL_SetWindowSize(window, std::max(1, int(std::lround(options.width / density))),
                              std::max(1, int(std::lround(options.height / density))));
    }
    int pixelW = options.width, pixelH = options.height;
    SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    style();
    ImGui_ImplSDL3_InitForOther(window);
    Renderer renderer;
    if (!renderer.init(window, static_cast<uint32_t>(pixelW), static_cast<uint32_t>(pixelH), options)) {
        SDL_Log("bgfx initialization or shaders failed");
        renderer.shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    renderer.setQuality(options.lowQuality ? RenderQuality::Low : RenderQuality::Balanced);
    renderer.setBloom(!options.noBloom);
    renderer.setDebug(RenderDebug(options.debugView));
    if (!options.renderBenchmark.empty())
        renderer.setVsync(false);
    Engine engine;
    engine.loadProject(options.project);
    Network network;
    if (options.host)
        network.host(options.port);
    if (!options.connectAddress.empty())
        network.connect(options.connectAddress, options.port);
    Session state;
    if (options.editor && options.stressPreview) {
        state.playSnapshot = encodeWorld(engine.world);
        state.playing = true;
        state.stress = true;
    }
    if (!engine.world.objects.empty())
        state.selected = engine.world.objects.begin()->first;
    state.material = 2;
    state.showStats = options.editor;
    state.showEvents = options.editor;
    Camera camera;
    const std::string game = lower(engine.projectName());
    camera.topDown = !options.editor && (game.find("construction") != std::string::npos ||
                                         game.find("top") != std::string::npos);
    if (options.cameraOverride) {
        camera.position = options.cameraPosition;
        camera.yaw = options.cameraYaw;
        camera.pitch = options.cameraPitch;
        camera.topDown = false;
    }
    const bool benchmarking = !options.renderBenchmark.empty();
    const bool characterMode = engine.playerSettings().enabled && !options.editor &&
                               !benchmarking && !options.renderSmoke && !options.cameraOverride;
    if (characterMode && !network.isClient())
        engine.addPlayer(1);
    if (characterMode) {
        camera.topDown = false;
        const auto &spawn = engine.playerSettings().spawns.front();
        camera.position = spawn.position + Vec3{0, 1.65f, 0};
        camera.yaw = spawn.yaw;
        camera.pitch = spawn.pitch;
    }
    PlayerInput pendingPlayerInput;
    uint64_t playerSequence = 0;
    uint32_t playerGeneration = 0;
    bool waitingForSpawn = true;
    Vec3 visualCorrection{};
    std::optional<PlayerState> previousPredicted;
    // Build the initial structural/collision caches before the timed warm-up. The tiny fixed
    // first step also initializes the gameplay clock without consuming scripted events.
    if (benchmarking)
        engine.step(1.f / 60.f);
    RenderBenchmark benchmark(options, pixelW, pixelH);
    const auto smokeWorld =
        options.renderSmoke ? encodeWorld(engine.world) : std::vector<uint8_t>{};
    const Camera smokeCamera = camera;
    EnvironmentSettings smokeEnvironment;
    Material smokeMaterial;
    LightId smokeLight{}, smokeNextLight{};
    int landscapeSmokeStage = 0;
    EntityId landscapeSmokeEntity{};
    uint64_t landscapeSmokeCommand{}, landscapeSmokeOriginal{};
    uint32_t landscapeSmokeRebuilds{};
    size_t landscapeSmokeHistory{};
    auto benchmarkStart = std::chrono::steady_clock::now();
    double benchmarkElapsed = 0;
    bool running = true, mouseLook = characterMode;
    SDL_SetWindowRelativeMouseMode(window, mouseLook);
    auto previous = std::chrono::steady_clock::now();
    float accumulator = 0;
    int frames = 0;
    bool captured = false;
    while (running) {
        if (options.renderSmoke) {
            if (frames == 8)
                SDL_SetWindowSize(window, 640, 360);
            if (frames == 16)
                renderer.setQuality(RenderQuality::Low);
            if (frames == 20) {
                smokeEnvironment = engine.world.environment;
                smokeMaterial = engine.world.materials.at(1);
                smokeNextLight = engine.world.nextLight;
                snapshot(engine, state);
                auto environment = smokeEnvironment;
                environment.exposure += 1;
                engine.setEnvironment(environment);
                auto material = smokeMaterial;
                material.roughness = .42f;
                engine.setMaterial(1, material);
                smokeLight = engine.addLight({});
            }
            if (frames == 24) {
                undo(engine, state);
                renderer.invalidateMeshes();
                if (engine.world.environment.exposure != smokeEnvironment.exposure ||
                    engine.world.materials.at(1).roughness != smokeMaterial.roughness ||
                    engine.world.lights.contains(smokeLight) ||
                    engine.world.nextLight != smokeNextLight)
                    throw std::runtime_error("Lighting authoring undo smoke check failed");
            }
            if (frames == 28) {
                redo(engine, state);
                renderer.invalidateMeshes();
                if (engine.world.environment.exposure != smokeEnvironment.exposure + 1 ||
                    engine.world.materials.at(1).roughness != .42f ||
                    !engine.world.lights.contains(smokeLight) ||
                    engine.world.nextLight != smokeNextLight + 1)
                    throw std::runtime_error("Lighting authoring redo smoke check failed");
            }
            if (frames == 32) {
                SDL_SetWindowSize(window, 1024, 640);
                renderer.setQuality(RenderQuality::Balanced);
            }
            if (frames == 36 && options.editor) {
                auto materials = engine.world.materials;
                engine.world = World{};
                engine.world.materials = std::move(materials);
                engine.resetPhysics();
                engine.simulating = false;
                state.playing = false;
                state.history = UndoHistory{};
                state.showLandscape = true;
                auto &settings = state.landscapeSettings;
                settings.minimum = {0, -8, 0};
                settings.maximum = {64, 8, 8};
                settings.surfaceHeight = 4;
                settings.hillAmplitude = 0;
                settings.overhangStrength = 0;
                settings.caveThreshold = 1;
                state.landscapeUndo = encodeWorld(engine.world);
                state.landscapeStarted = ImGui::GetTime();
                state.landscapeCommand = engine.requestLandscape(settings);
                renderer.invalidateMeshes();
                landscapeSmokeStage = 1;
            }
            if (frames == 48)
                camera.position = camera.position + Vec3{80, 30, 80};
            if (frames == 56)
                camera = smokeCamera;
            if (frames == 64)
                SDL_MinimizeWindow(window);
            if (frames == 68)
                SDL_RestoreWindow(window);
            if (frames == 76) {
                if (options.editor && landscapeSmokeStage != 7)
                    throw std::runtime_error(
                        "Landscape rendering smoke did not complete before scene replacement");
                engine.world = decodeWorld(smokeWorld);
                engine.resetPhysics();
                renderer.invalidateMeshes();
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if ((benchmarking || options.ptCaptureSamples) && event.type != SDL_EVENT_QUIT &&
                event.type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                continue;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
                benchmark.resized(pixelW, pixelH);
                renderer.resize(pixelW, pixelH);
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                mouseLook = false;
                pendingPlayerInput = {};
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB && !options.editor &&
                !characterMode) {
                camera.topDown = !camera.topDown;
                mouseLook = false;
                SDL_SetWindowRelativeMouseMode(window, mouseLook);
            }
            if (event.type == SDL_EVENT_KEY_DOWN && options.editor && event.key.key == SDLK_F5 &&
                !state.playing && !network.isClient()) {
                state.playSnapshot = encodeWorld(engine.world);
                state.playing = true;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && options.editor && event.key.key == SDLK_F6 &&
                state.playing && !network.isClient()) {
                engine.world = decodeWorld(state.playSnapshot);
                renderer.invalidateMeshes();
                engine.resetPhysics();
                state.playing = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && options.editor && event.key.key == SDLK_S &&
                (event.key.mod & SDL_KMOD_CTRL)) {
                saveWorld(engine.world, options.project / "scene.aspera");
            }
            if (characterMode && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                mouseLook) {
                if (event.key.key == SDLK_SPACE)
                    pendingPlayerInput.jump = true;
                if (event.key.key == SDLK_R)
                    pendingPlayerInput.recover = true;
            }
            if (characterMode && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (mouseLook) {
                    if (event.button.button == SDL_BUTTON_LEFT)
                        pendingPlayerInput.damage = true;
                    if (event.button.button == SDL_BUTTON_RIGHT)
                        pendingPlayerInput.build = true;
                } else if (!ImGui::GetIO().WantCaptureMouse &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    mouseLook = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
            }
            if (!characterMode && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                !ImGui::GetIO().WantCaptureMouse) {
                if (event.button.button ==
                    (options.editor ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE)) {
                    mouseLook = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
                if (event.button.button == SDL_BUTTON_LEFT && !mouseLook) {
                    if (options.editor)
                        snapshot(engine, state);
                    if (options.editor)
                        actionAtPointer(engine, network, state, camera, event.button.x,
                                        event.button.y, false);
                    else {
                        Command c;
                        c.type = CommandType::SphereDamage;
                        c.radius = .45f;
                        c.strength = 1.f;
                        if (auto hit = raycastWorld(
                                engine.world, camera.eye(),
                                camera.aimDirection(event.button.x, event.button.y), 100.f)) {
                            c.entity = hit->entity;
                            c.point = hit->worldPoint;
                            submit(engine, network, c);
                        }
                    }
                }
                if (!options.editor && event.button.button == SDL_BUTTON_RIGHT && !mouseLook) {
                    state.tool = Tool::Prefab;
                    actionAtPointer(engine, network, state, camera, event.button.x, event.button.y,
                                    false);
                }
            }
            if ((!characterMode && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                 event.button.button == (options.editor ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE)) ||
                event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                mouseLook = false;
                pendingPlayerInput = {};
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION && mouseLook) {
                camera.yaw += event.motion.xrel * .0035f;
                camera.pitch = std::clamp(camera.pitch - event.motion.yrel * .0035f, -1.45f, 1.45f);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        double frameMs = std::chrono::duration<double, std::milli>(now - previous).count();
        benchmarkElapsed =
            std::chrono::duration<double>(now - benchmarkStart).count() - options.warmupSeconds;
        float delta = options.fixedStep ? 1.f / 60.f : std::min(.1f, float(frameMs / 1000.));
        previous = now;
        accumulator += delta;
        const bool *keys = SDL_GetKeyboardState(nullptr);
        Vec3 move{};
        if (!options.editor || mouseLook) {
            if (keys[SDL_SCANCODE_W])
                move = move + camera.forward();
            if (keys[SDL_SCANCODE_S])
                move = move - camera.forward();
            if (keys[SDL_SCANCODE_A])
                move = move - camera.right();
            if (keys[SDL_SCANCODE_D])
                move = move + camera.right();
        }
        if (camera.topDown) {
            move.y = 0;
        }
        if (!characterMode && !options.ptCaptureSamples)
            camera.position = camera.position + normalized(move) * delta * 5.f;
        engine.setInput("move_x", move.x);
        engine.setInput("move_z", move.z);
        if (benchmarking) {
            double t = std::max(0., benchmarkElapsed);
            const bool large = options.project.filename() == "lighting-benchmark";
            float radius = large ? 28.f : 8.6f;
            float angle = .62f + float(t) * .10f;
            camera.topDown = false;
            camera.position = {std::sin(angle) * radius, large ? 12.f : 4.f,
                               -std::cos(angle) * radius};
            Vec3 aim = normalized(Vec3{0, 1.5f, large ? 8.f : 0.f} - camera.position);
            camera.yaw = std::atan2(aim.x, aim.z);
            camera.pitch = std::asin(aim.y);
        }
        engine.simulating =
            benchmarking ? benchmarkElapsed >= 0 : (!options.editor || state.playing);
        network.poll(engine);
        if (options.ptCaptureSamples) accumulator = 0;
        while (accumulator >= 1.f / 60.f) {
            if (characterMode) {
                const PlayerId localId = network.isClient() ? network.localPlayer() : 1;
                auto found = engine.players().find(localId);
                if (found != engine.players().end()) {
                    if (playerGeneration != found->second.generation) {
                        playerGeneration = found->second.generation;
                        playerSequence = 0;
                        waitingForSpawn = true;
                        pendingPlayerInput = {};
                        visualCorrection = {};
                        previousPredicted.reset();
                        camera.yaw = found->second.yaw;
                        camera.pitch = found->second.pitch;
                    }
                    if (waitingForSpawn && !found->second.spawnBlocked) {
                        camera.yaw = found->second.yaw;
                        camera.pitch = found->second.pitch;
                        waitingForSpawn = false;
                    }
                    PlayerInput input = pendingPlayerInput;
                    input.sequence = ++playerSequence;
                    input.generation = playerGeneration;
                    input.yaw = camera.yaw;
                    input.pitch = camera.pitch;
                    if (mouseLook) {
                        input.moveX = float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]);
                        input.moveZ = float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]);
                        float magnitude =
                            std::sqrt(input.moveX * input.moveX + input.moveZ * input.moveZ);
                        if (magnitude > 1) {
                            input.moveX /= magnitude;
                            input.moveZ /= magnitude;
                        }
                    }
                    if (network.isClient()) {
                        network.sendPlayerInput(input);
                        engine.predictPlayer(localId, input);
                    } else
                        engine.submitPlayerInput(localId, input);
                    pendingPlayerInput = {};
                }
            }
            if (!network.isClient()) {
                engine.step();
                if (options.host)
                    network.broadcast(engine);
            }
            accumulator -= 1.f / 60.f;
        }
        if (characterMode) {
            const PlayerId localId = network.isClient() ? network.localPlayer() : 1;
            auto pose =
                network.isClient() ? engine.predictedPlayer() : std::optional<PlayerState>{};
            if (!pose && engine.players().contains(localId))
                pose = engine.players().at(localId);
            if (pose) {
                if (previousPredicted && pose->generation == previousPredicted->generation) {
                    Vec3 expected =
                        previousPredicted->position + previousPredicted->velocity * delta;
                    Vec3 correction = expected - pose->position;
                    if (network.isClient() && length(correction) > .02f && length(correction) < .5f)
                        visualCorrection = visualCorrection + correction;
                    if (length(correction) >= .5f)
                        visualCorrection = {};
                }
                visualCorrection = visualCorrection * std::max(0.f, 1.f - delta / .1f);
                if (length(visualCorrection) > .5f)
                    visualCorrection = {};
                camera.position = pose->position + Vec3{0, 1.65f, 0} + visualCorrection;
                previousPredicted = pose;
            }
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!options.noUi && !renderer.timings().fallbackReason.empty()) {
            ImGui::SetNextWindowPos({12, ImGui::GetIO().DisplaySize.y - 12}, ImGuiCond_Always, {0,1});
            ImGui::SetNextWindowSize({420,0}, ImGuiCond_Always);
            ImGui::Begin("Renderer fallback", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
            ImGui::TextWrapped("Using raster: %s",renderer.timings().fallbackReason.c_str());
            ImGui::End();
        }
        bool restartRequested = false;
        if (options.editor)
            landscapeCompletion(engine, state);
        if (!options.noUi && options.editor)
            editorUi(engine, network, options, state, renderer, camera);
        else if (!options.noUi)
            restartRequested = playerUi(engine, network, state, camera, characterMode);
        if (!options.noUi && options.editor) {
            lightingUi(engine, network, state, renderer, camera);
            landscapeUi(engine, network, state);
        }
        if (characterMode && state.recoverRequested) {
            pendingPlayerInput.recover = true;
            state.recoverRequested = false;
        }
        if (!options.noUi && state.showStats) {
            ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 12, 410}, ImGuiCond_Always,
                                    {1, 0});
            ImGui::SetNextWindowSize({308, 315}, ImGuiCond_FirstUseEver);
            ImGui::Begin("PERFORMANCE", &state.showStats, ImGuiWindowFlags_NoMove);
            ImGui::Text("Impacts: %u pending, %u applied", engine.stats.pendingImpacts,
                        engine.stats.processedImpacts);
            ImGui::Text("Impact overflow: %u | clipped steps: %llu", engine.stats.droppedImpacts,
                        static_cast<unsigned long long>(engine.stats.droppedSimulationSteps));
            ImGui::Text("Pending edits/generation: %u", engine.stats.pendingCommands);
            ImGui::Text("Frame %.2f ms  Physics %.2f ms", engine.stats.frameMs,
                        engine.stats.physicsMs);
            ImGui::Text("Nodes %u  Bonds %u  Mesh %.2f ms", engine.stats.structuralNodes,
                        engine.stats.bonds, engine.stats.meshMs);
            ImGui::TextWrapped("Structure %.2f ms | prepare %.2f | connectivity %.2f",
                               engine.stats.structureMs, engine.stats.structurePreparationMs,
                               engine.stats.structureConnectivityMs);
            ImGui::TextWrapped("Solve %.2f ms | fragments %.2f | physics publish %.2f",
                               engine.stats.structureSolveMs, engine.stats.structureFragmentMs,
                               engine.stats.physicsPublicationMs);
            ImGui::TextWrapped(
                "Structure regions %llu | cells scanned %llu",
                static_cast<unsigned long long>(engine.stats.structuralRegionRebuilds),
                static_cast<unsigned long long>(engine.stats.structuralCellsScanned));
            ImGui::TextWrapped("Structure cache hits %llu | %.1f MiB",
                               static_cast<unsigned long long>(engine.stats.structuralCacheHits),
                               double(engine.stats.structuralCacheBytes) / (1024 * 1024));
            const auto rendering = renderer.timings();
            ImGui::Text("Renderer: %s", rendering.backend.c_str());
            if (!rendering.fallbackReason.empty()) ImGui::TextWrapped("%s",rendering.fallbackReason.c_str());
            if(rendering.ptBounces) ImGui::Text("PT %.2f ms | denoise %.2f ms | AS %.2f ms | %.1f MiB",rendering.traceMs,rendering.denoiseMs,rendering.accelerationMs,rendering.ptBytes/1048576.);
            ImGui::Text("GPU %.2f ms | GI %.2f ms", rendering.gpuMs, rendering.giMs);
            ImGui::Text("Draws %u | GI bricks updated %u", rendering.drawCalls,
                        rendering.pendingGi);
            ImGui::Text("Mesh chunks %u | rebuilt %u | jobs %u + %u", rendering.cachedChunks,
                        rendering.rebuiltChunks, rendering.pendingMeshJobs,
                        rendering.queuedMeshJobs);
            ImGui::Text("Mesh prepare %.2f ms | GPU geometry %.1f MiB", rendering.meshPrepareMs,
                        double(rendering.geometryBytes) / (1024 * 1024));
            ImGui::Text("Collision bodies %u | rebuilt chunks %llu",
                        engine.stats.collisionBodyCount,
                        static_cast<unsigned long long>(engine.stats.collisionChunkRebuilds));
            ImGui::Text("Collision prepare %.2f ms | cache %.1f MiB",
                        engine.stats.collisionPreparationMs,
                        double(engine.stats.collisionCacheBytes) / (1024 * 1024));
            ImGui::End();
        }
        if (!options.noUi && state.showEvents) {
            ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 12, 737}, ImGuiCond_Always,
                                    {1, 0});
            ImGui::SetNextWindowSize({308, 260}, ImGuiCond_FirstUseEver);
            ImGui::Begin("EVENT LOG", &state.showEvents, ImGuiWindowFlags_NoMove);
            for (const Event &e : engine.events)
                ImGui::Text("[%llu] %s", static_cast<unsigned long long>(e.tick),
                            e.message.c_str());
            ImGui::End();
        }
        ImGui::Render();
        float cameraView[16];
        camera.view(cameraView);
        renderer.begin(camera.eye(), cameraView);
        renderer.drawWorld(
            engine.world, options.editor && state.stress,
            engine.playerSettings().enabled
                ? (network.isClient() ? network.presentedPlayers() : engine.players())
                : std::map<PlayerId, PlayerState>{},
            characterMode ? (network.isClient() ? network.localPlayer() : 1) : 0);
        renderer.drawImGui(ImGui::GetDrawData());
        if (!options.screenshot.empty() && !captured &&
            (options.ptCaptureSamples ? renderer.accumulatedSamples() >= uint32_t(options.ptCaptureSamples)
                                      : frames >= std::max(4, options.frameLimit - 2))) {
            renderer.screenshot(options.screenshot);
            captured = true;
            if (options.ptCaptureSamples) running = false;
        }
        renderer.end();
        if (options.renderSmoke && options.editor && landscapeSmokeStage > 0 &&
            landscapeSmokeStage < 7) {
            const auto timing = renderer.timings();
            const bool idle = timing.pendingMeshJobs == 0 && timing.queuedMeshJobs == 0 &&
                              engine.stats.pendingCommands == 0;
            auto completed = [&] {
                for (auto &event : engine.events)
                    if (event.command == landscapeSmokeCommand) {
                        if (event.type == "rejected")
                            throw std::runtime_error("Landscape smoke edit rejected: " +
                                                     event.message);
                        if (event.type == "committed")
                            return true;
                    }
                return false;
            };
            auto requestGeneration = [&] {
                state.landscapeUndo = encodeWorld(engine.world);
                state.landscapeStarted = ImGui::GetTime();
                state.landscapeCommand = engine.requestLandscape(state.landscapeSettings);
            };
            if (landscapeSmokeStage == 1 && !state.landscapeCommand && idle) {
                if (engine.world.objects.size() != 1 || state.history.count() != 1 ||
                    timing.cachedChunks != 4)
                    throw std::runtime_error(
                        "Landscape generation/undo/chunk publication smoke check failed");
                landscapeSmokeEntity = state.selected;
                landscapeSmokeOriginal = engine.world.objects.at(state.selected).grid.checksum();
                snapshot(engine, state);
                Command paint;
                paint.entity = state.selected;
                paint.type = CommandType::Paint;
                paint.cell = {2, -6, 2};
                paint.material = 2;
                landscapeSmokeCommand = engine.submit(paint);
                landscapeSmokeRebuilds = 0;
                landscapeSmokeStage = 2;
            } else if (landscapeSmokeStage == 2 || landscapeSmokeStage == 3) {
                landscapeSmokeRebuilds += timing.rebuiltChunks;
                if (completed() && idle) {
                    if (landscapeSmokeRebuilds != uint32_t(landscapeSmokeStage - 1))
                        throw std::runtime_error("Landscape edit rebuilt unrelated chunks");
                    if (landscapeSmokeStage == 2) {
                        snapshot(engine, state);
                        Command cut;
                        cut.entity = landscapeSmokeEntity;
                        cut.type = CommandType::SetVoxel;
                        cut.cell = {31, -6, 2};
                        cut.material = 0;
                        landscapeSmokeCommand = engine.submit(cut);
                        landscapeSmokeRebuilds = 0;
                        landscapeSmokeStage = 3;
                    } else {
                        undo(engine, state);
                        undo(engine, state);
                        if (engine.world.objects.at(landscapeSmokeEntity).grid.checksum() !=
                            landscapeSmokeOriginal)
                            throw std::runtime_error(
                                "Landscape undo did not restore original cells");
                        redo(engine, state);
                        redo(engine, state);
                        if (engine.world.objects.at(landscapeSmokeEntity).grid.get({31, -6, 2}) !=
                            0)
                            throw std::runtime_error("Landscape redo did not restore excavation");
                        renderer.invalidateMeshes();
                        landscapeSmokeStage = 4;
                    }
                }
            } else if (landscapeSmokeStage == 4 && idle) {
                const auto saved = encodeWorld(engine.world);
                landscapeSmokeHistory = state.history.count();
                requestGeneration();
                engine.world = decodeWorld(saved);
                engine.resetPhysics();
                renderer.invalidateMeshes();
                landscapeSmokeStage = 5;
            } else if (landscapeSmokeStage == 5 && !state.landscapeCommand && idle) {
                if (engine.world.objects.size() != 1 ||
                    state.history.count() != landscapeSmokeHistory)
                    throw std::runtime_error(
                        "Cancelled generation changed the scene or undo history");
                ++state.landscapeSettings.seed;
                requestGeneration();
                landscapeSmokeStage = 6;
            } else if (landscapeSmokeStage == 6 && !state.landscapeCommand && idle) {
                if (engine.world.objects.size() != 2 ||
                    state.history.count() != landscapeSmokeHistory + 1)
                    throw std::runtime_error("Regeneration after cancellation failed");
                landscapeSmokeStage = 7;
            }
        }
        if (benchmarking && benchmarkElapsed >= 0) {
            benchmark.record(benchmarkElapsed, frameMs, engine.stats.frameMs, renderer.timings());
            if (benchmarkElapsed >= options.benchmarkSeconds)
                running = false;
        }
        if (restartRequested) {
            engine.loadProject(options.project);
            if (characterMode)
                engine.addPlayer(1);
            network.resetRound(engine);
            playerGeneration = 0;
            previousPredicted.reset();
            visualCorrection = {};
            renderer.invalidateMeshes();
            state = {};
            state.material = 2;
            state.showStats = false;
            state.showEvents = false;
            if (!engine.world.objects.empty())
                state.selected = engine.world.objects.begin()->first;
        }
        ++frames;
        if (options.frameLimit > 0 && frames >= options.frameLimit && !options.ptCaptureSamples)
            running = false;
    }
    if (benchmarking)
        benchmark.write();
    if (options.renderSmoke && frames >= 96)
        std::printf(
            "Render smoke passed: lighting undo/redo, quality/size changes, camera teleport, "
            "minimize/restore, scene replacement, and editor landscape/chunk lifecycle (%dx%d "
            "final pixels).\n",
            pixelW, pixelH);
    renderer.shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
} // namespace aspera
