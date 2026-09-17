#include "internal.hpp"
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace aspera {
struct Scripts::Impl {
    lua_State *L{};
    Engine *engine{};
    bool initializing{}, disabled{};
    uint64_t lastEventSequence{};
    std::map<std::string, float> inputs;
    ~Impl() {
        if (L)
            lua_close(L);
    }
    static Impl &self(lua_State *L) {
        return *static_cast<Impl *>(lua_touserdata(L, lua_upvalueindex(1)));
    }
    static void hook(lua_State *L, lua_Debug *) {
        luaL_error(L, "Script instruction budget exceeded");
    }
    static bool fieldNumber(lua_State *L, int table, const char *key, float &value) {
        lua_getfield(L, table, key);
        bool present = !lua_isnil(L, -1);
        if (present) {
            if (!lua_isnumber(L, -1))
                luaL_error(L, "lighting fields must be numbers");
            value = float(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        return present;
    }
    static void fieldVec(lua_State *L, int table, const char *key, Vec3 &value) {
        lua_getfield(L, table, key);
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1))
                luaL_error(L, "lighting vector must be a table");
            int v = lua_gettop(L);
            fieldNumber(L, v, "x", value.x);
            fieldNumber(L, v, "y", value.y);
            fieldNumber(L, v, "z", value.z);
        }
        lua_pop(L, 1);
    }
    static int environment(lua_State *L) {
        auto &engine = *self(L).engine;
        luaL_checktype(L, 1, LUA_TTABLE);
        auto value = engine.world.environment;
        fieldVec(L, 1, "sun_direction", value.sunDirection);
        fieldVec(L, 1, "sun_color", value.sunColor);
        fieldVec(L, 1, "sky_color", value.skyColor);
        fieldNumber(L, 1, "sun_intensity", value.sunIntensity);
        fieldNumber(L, 1, "sky_intensity", value.skyIntensity);
        fieldNumber(L, 1, "exposure", value.exposure);
        fieldNumber(L, 1, "bloom_strength", value.bloomStrength);
        try {
            engine.setEnvironment(value);
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
        return 0;
    }
    static int material(lua_State *L) {
        auto &engine = *self(L).engine;
        auto id = luaL_checkinteger(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (id < 0 || id >= lua_Integer(engine.world.materials.size()))
            return luaL_error(L, "Invalid material");
        auto value = engine.world.materials[MaterialId(id)];
        lua_getfield(L, 2, "name");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1))
                return luaL_error(L, "material name must be a string");
            value.name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "rgba");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1))
                return luaL_error(L, "rgba must be an integer");
            auto rgba = lua_tointeger(L, -1);
            if (rgba < 0 || uint64_t(rgba) > UINT32_MAX)
                return luaL_error(L, "rgba out of range");
            value.rgba = uint32_t(rgba);
        }
        lua_pop(L, 1);
        fieldNumber(L, 2, "density", value.density);
        fieldNumber(L, 2, "resistance", value.resistance);
        fieldNumber(L, 2, "friction", value.friction);
        fieldNumber(L, 2, "tension", value.tension);
        fieldNumber(L, 2, "compression", value.compression);
        fieldNumber(L, 2, "shear", value.shear);
        fieldNumber(L, 2, "bending", value.bending);
        fieldNumber(L, 2, "impact_toughness", value.impactToughness);
        fieldNumber(L, 2, "roughness", value.roughness);
        fieldNumber(L, 2, "metallic", value.metallic);
        fieldVec(L, 2, "emissive", value.emissive);
        fieldNumber(L, 2, "emission_intensity", value.emissionIntensity);
        try {
            engine.setMaterial(MaterialId(id), value);
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
        return 0;
    }
    static Light lightTable(lua_State *L, int table, Light value) {
        lua_getfield(L, table, "type");
        if (!lua_isnil(L, -1)) {
            const char *type = lua_tostring(L, -1);
            if (!type || (std::string(type) != "point" && std::string(type) != "spot"))
                luaL_error(L, "light type must be point or spot");
            value.type = std::string(type) == "spot" ? LightType::Spot : LightType::Point;
        }
        lua_pop(L, 1);
        fieldVec(L, table, "position", value.position);
        fieldVec(L, table, "direction", value.direction);
        fieldVec(L, table, "color", value.color);
        fieldNumber(L, table, "intensity", value.intensity);
        fieldNumber(L, table, "range", value.range);
        fieldNumber(L, table, "inner_angle", value.innerAngle);
        fieldNumber(L, table, "outer_angle", value.outerAngle);
        lua_getfield(L, table, "shadows");
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1))
                luaL_error(L, "shadows must be boolean");
            value.shadows = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, table, "shadow_priority");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1))
                luaL_error(L, "shadow_priority must be integer");
            auto priority = lua_tointeger(L, -1);
            if (priority < -100000 || priority > 100000)
                luaL_error(L, "shadow_priority out of range");
            value.shadowPriority = int32_t(priority);
        }
        lua_pop(L, 1);
        return value;
    }
    static int addLight(lua_State *L) {
        auto &engine = *self(L).engine;
        luaL_checktype(L, 1, LUA_TTABLE);
        try {
            lua_pushinteger(L, lua_Integer(engine.addLight(lightTable(L, 1, {}))));
            return 1;
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
    }
    static int updateLight(lua_State *L) {
        auto &engine = *self(L).engine;
        auto id = LightId(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        auto it = engine.world.lights.find(id);
        if (it == engine.world.lights.end())
            return luaL_error(L, "Unknown light");
        try {
            engine.updateLight(id, lightTable(L, 2, it->second));
            return 0;
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
    }
    static int removeLight(lua_State *L) {
        try {
            self(L).engine->removeLight(LightId(luaL_checkinteger(L, 1)));
            return 0;
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
    }
    static int landscape(lua_State *L) {
        auto &s = self(L);
        if (!s.initializing)
            return luaL_error(L, "generate_landscape is scene-authoring only");
        luaL_checktype(L, 1, LUA_TTABLE);
        LandscapeSettings settings;
        auto integer = [&](int table, const char *key, int64_t value, int64_t lo, int64_t hi) {
            lua_getfield(L, table, key);
            if (!lua_isnil(L, -1))
                value = luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (value < lo || value > hi)
                luaL_error(L, "Invalid landscape integer: %s", key);
            return value;
        };
        settings.seed = uint32_t(integer(1, "seed", settings.seed, 0, UINT32_MAX));
        auto bounds = [&](const char *key, IVec3 &value) {
            lua_getfield(L, 1, key);
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                int table = lua_gettop(L);
                value.x = int(integer(table, "x", value.x, -1280, 1281));
                value.y = int(integer(table, "y", value.y, -640, 641));
                value.z = int(integer(table, "z", value.z, -1280, 1281));
            }
            lua_pop(L, 1);
        };
        bounds("minimum", settings.minimum);
        bounds("maximum", settings.maximum);
        fieldNumber(L, 1, "surface_height", settings.surfaceHeight);
        fieldNumber(L, 1, "hill_amplitude", settings.hillAmplitude);
        fieldNumber(L, 1, "feature_scale", settings.featureScale);
        fieldNumber(L, 1, "cave_threshold", settings.caveThreshold);
        fieldNumber(L, 1, "overhang_strength", settings.overhangStrength);
        settings.grassDepth = int(integer(1, "grass_depth", settings.grassDepth, 0, 32));
        settings.soilDepth = int(integer(1, "soil_depth", settings.soilDepth, 0, 64));
        settings.bottomDepth = int(integer(1, "bottom_depth", settings.bottomDepth, 1, 511));
        settings.grassMaterial =
            MaterialId(integer(1, "grass_material", settings.grassMaterial, 1, 65535));
        settings.soilMaterial =
            MaterialId(integer(1, "soil_material", settings.soilMaterial, 1, 65535));
        settings.rockMaterial =
            MaterialId(integer(1, "rock_material", settings.rockMaterial, 1, 65535));
        try {
            auto id = s.engine->generateLandscape(settings);
            lua_pushinteger(L, lua_Integer(id));
            return 1;
        } catch (const std::exception &ex) {
            return luaL_error(L, "%s", ex.what());
        }
    }
    static int spawn(lua_State *L) {
        auto &s = self(L);
        if (!s.initializing)
            return luaL_error(L, "spawn_box is scene-authoring only; use prefab during gameplay");
        const char *name = luaL_checkstring(L, 1);
        Vec3 pos{float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
                 float(luaL_checknumber(L, 4))};
        int nx = int(luaL_checkinteger(L, 5)), ny = int(luaL_checkinteger(L, 6)),
            nz = int(luaL_checkinteger(L, 7));
        int mat = int(luaL_optinteger(L, 8, 1));
        bool anchored = lua_toboolean(L, 9);
        if (nx < 1 || ny < 1 || nz < 1 || nx > 512 || ny > 512 || nz > 512 ||
            int64_t(nx) * ny * nz > 2000000 || mat < 1 ||
            mat >= int(s.engine->world.materials.size()))
            return luaL_error(L, "Invalid box dimensions/material");
        VoxelGrid grid;
        for (int x = 0; x < nx; ++x)
            for (int y = 0; y < ny; ++y)
                for (int z = 0; z < nz; ++z)
                    grid.set({x, y, z}, MaterialId(mat));
        auto id = s.engine->world.spawn(name, std::move(grid), {pos, {}}, !anchored);
        if (anchored)
            for (int x = 0; x < nx; ++x)
                for (int z = 0; z < nz; ++z)
                    s.engine->world.objects.at(id).anchors.insert({x, 0, z});
        lua_pushinteger(L, lua_Integer(id));
        return 1;
    }
    static int set(lua_State *L) {
        auto &s = self(L);
        Command c;
        c.entity = EntityId(luaL_checkinteger(L, 1));
        c.cell = {int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3)),
                  int(luaL_checkinteger(L, 4))};
        auto material = luaL_checkinteger(L, 5);
        if (material < 0 || material >= lua_Integer(s.engine->world.materials.size()))
            return luaL_error(L, "Invalid material");
        c.material = MaterialId(material);
        if (s.initializing) {
            auto it = s.engine->world.objects.find(c.entity);
            if (it == s.engine->world.objects.end())
                return luaL_error(L, "Unknown object");
            it->second.grid.set(c.cell, c.material);
            if (!c.material)
                it->second.anchors.erase(c.cell);
            ++it->second.revision;
            lua_pushinteger(L, 0);
        } else
            lua_pushinteger(L, s.engine->submit(c));
        return 1;
    }
    static int damage(lua_State *L) {
        auto &s = self(L);
        Command c;
        c.type = CommandType::SphereDamage;
        c.entity = EntityId(luaL_checkinteger(L, 1));
        c.point = {float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
                   float(luaL_checknumber(L, 4))};
        c.radius = float(luaL_checknumber(L, 5));
        c.strength = float(luaL_optnumber(L, 6, 8));
        lua_pushinteger(L, s.engine->submit(c));
        return 1;
    }
    static int release(lua_State *L) {
        Command c;
        c.type = CommandType::ReleaseBody;
        c.entity = EntityId(luaL_checkinteger(L, 1));
        c.vector = {float(luaL_optnumber(L, 2, 0)), float(luaL_optnumber(L, 3, 0)),
                    float(luaL_optnumber(L, 4, 0))};
        lua_pushinteger(L, self(L).engine->submit(c));
        return 1;
    }
    static int prefab(lua_State *L) {
        Command c;
        c.type = CommandType::SpawnPrefab;
        c.point = {float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                   float(luaL_checknumber(L, 3))};
        c.radius = float(luaL_optnumber(L, 4, .5));
        c.material = MaterialId(luaL_optinteger(L, 5, 2));
        lua_pushinteger(L, self(L).engine->submit(c));
        return 1;
    }
    static int prop(lua_State *L) {
        auto &props = self(L).engine->world.properties;
        std::string key = luaL_checkstring(L, 1);
        if (key.size() > 128)
            return luaL_error(L, "Property name too long");
        if (lua_gettop(L) > 1) {
            double v = luaL_checknumber(L, 2);
            if (!std::isfinite(v))
                return luaL_error(L, "Non-finite property");
            if (!props.contains(key) && props.size() >= 4096)
                return luaL_error(L, "Property budget reached");
            props[key] = v;
        }
        auto it = props.find(key);
        lua_pushnumber(L, it == props.end() ? 0 : it->second);
        return 1;
    }
    static int count(lua_State *L) {
        auto &w = self(L).engine->world;
        int material = int(luaL_optinteger(L, 1, -1));
        size_t count = 0;
        for (auto &[id, o] : w.objects)
            for (auto [p, m] : o.grid.occupied())
                count += (material < 0 || m == material);
        lua_pushinteger(L, count);
        return 1;
    }
    static int position(lua_State *L) {
        auto &w = self(L).engine->world;
        auto it = w.objects.find(EntityId(luaL_checkinteger(L, 1)));
        if (it == w.objects.end()) {
            lua_pushnil(L);
            return 1;
        }
        auto p = it->second.transform.position;
        lua_pushnumber(L, p.x);
        lua_pushnumber(L, p.y);
        lua_pushnumber(L, p.z);
        return 3;
    }
    static int entities(lua_State *L) {
        lua_newtable(L);
        int index = 1;
        for (auto &[id, o] : self(L).engine->world.objects) {
            lua_pushinteger(L, id);
            lua_rawseti(L, -2, index++);
        }
        return 1;
    }
    static int object(lua_State *L) {
        auto &w = self(L).engine->world;
        auto it = w.objects.find(EntityId(luaL_checkinteger(L, 1)));
        if (it == w.objects.end()) {
            lua_pushnil(L);
            return 1;
        }
        auto &o = it->second;
        lua_newtable(L);
        lua_pushstring(L, o.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, o.id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, o.grid.count());
        lua_setfield(L, -2, "voxels");
        lua_pushboolean(L, o.dynamic);
        lua_setfield(L, -2, "dynamic");
        lua_pushnumber(L, o.stress);
        lua_setfield(L, -2, "stress");
        return 1;
    }
    static int anchor(lua_State *L) {
        Command c;
        c.type = CommandType::Anchor;
        c.entity = EntityId(luaL_checkinteger(L, 1));
        c.cell = {int(luaL_checkinteger(L, 2)), int(luaL_checkinteger(L, 3)),
                  int(luaL_checkinteger(L, 4))};
        c.strength = lua_toboolean(L, 5) ? 1 : 0;
        lua_pushinteger(L, self(L).engine->submit(c));
        return 1;
    }
    static int input(lua_State *L) {
        lua_pushnumber(L, self(L).inputs[luaL_checkstring(L, 1)]);
        return 1;
    }
    static int ray(lua_State *L) {
        Vec3 origin{float(luaL_checknumber(L, 1)), float(luaL_checknumber(L, 2)),
                    float(luaL_checknumber(L, 3))},
            direction{float(luaL_checknumber(L, 4)), float(luaL_checknumber(L, 5)),
                      float(luaL_checknumber(L, 6))};
        float distance = float(luaL_optnumber(L, 7, 100));
        if (!std::isfinite(distance) || distance <= 0 || distance > 1000)
            return luaL_error(L, "Invalid ray length");
        auto hit = raycastWorld(self(L).engine->world, origin, direction, distance);
        if (!hit) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, hit->entity);
        lua_pushnumber(L, hit->worldPoint.x);
        lua_pushnumber(L, hit->worldPoint.y);
        lua_pushnumber(L, hit->worldPoint.z);
        return 4;
    }
    static int log(lua_State *L) {
        self(L).engine->emit("script", 0, luaL_checkstring(L, 1));
        return 0;
    }
    static int impulse(lua_State *L) {
        Command c;
        c.type = CommandType::Impulse;
        c.entity = EntityId(luaL_checkinteger(L, 1));
        c.vector = {float(luaL_checknumber(L, 2)), float(luaL_checknumber(L, 3)),
                    float(luaL_checknumber(L, 4))};
        auto it = self(L).engine->world.objects.find(c.entity);
        if (it != self(L).engine->world.objects.end())
            c.point = it->second.transform.position;
        lua_pushinteger(L, self(L).engine->submit(c));
        return 1;
    }
    void bind(const char *key, lua_CFunction fn) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, key);
    }
    void create() {
        L = luaL_newstate();
        if (!L)
            throw std::runtime_error("Cannot create Lua VM");
        luaL_openlibs(L);
        for (auto key : {"io", "os", "package", "debug", "dofile", "loadfile", "require"}) {
            lua_pushnil(L);
            lua_setglobal(L, key);
        }
        lua_newtable(L);
        bind("spawn_box", spawn);
        bind("generate_landscape", landscape);
        bind("release_body", release);
        bind("voxel", set);
        bind("damage", damage);
        bind("prefab", prefab);
        bind("property", prop);
        bind("count_voxels", count);
        bind("position", position);
        bind("input", input);
        bind("log", log);
        bind("impulse", impulse);
        bind("raycast", ray);
        bind("entities", entities);
        bind("object", object);
        bind("anchor", anchor);
        bind("environment", environment);
        bind("material", material);
        bind("add_light", addLight);
        bind("update_light", updateLight);
        bind("remove_light", removeLight);
        lua_setglobal(L, "engine");
    }
    void file(const std::filesystem::path &path, int results = 0) {
        lua_sethook(L, hook, LUA_MASKCOUNT, 20000000);
        if (luaL_loadfile(L, path.string().c_str()) != LUA_OK ||
            lua_pcall(L, 0, results, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_sethook(L, nullptr, 0, 0);
            throw std::runtime_error(error);
        }
        lua_sethook(L, nullptr, 0, 0);
    }
};
Scripts::Scripts() : p(std::make_unique<Impl>()) {}
Scripts::~Scripts() = default;
void Scripts::load(Engine &engine, const std::filesystem::path &path, bool initialize) {
    auto replacement = std::make_unique<Impl>();
    replacement->engine = &engine;
    replacement->create();
    try {
        replacement->file(path / "project.lua", 1);
        if (!lua_istable(replacement->L, -1))
            throw std::runtime_error("project.lua must return manifest table");
        lua_getfield(replacement->L, -1, "name");
        const char *title = lua_tostring(replacement->L, -1);
        name = title ? title : path.filename().string();
        lua_pop(replacement->L, 2);
        replacement->initializing = initialize;
        if (initialize)
            replacement->file(path / "scene.lua");
        replacement->initializing = false;
        replacement->file(path / "server.lua");
        p = std::move(replacement);
        project = path;
        engine.emit("script", 0, "Loaded " + name);
    } catch (const std::exception &ex) {
        engine.emit("script_error", 0, ex.what());
        if (initialize)
            throw;
    }
}
void Scripts::tick(Engine &engine, float dt) {
    if (!p->L || p->disabled)
        return;
    std::vector<Event> pending;
    for (auto &ev : engine.events)
        if (ev.sequence > p->lastEventSequence)
            pending.push_back(ev);
    for (auto &ev : pending) {
        p->lastEventSequence = ev.sequence;
        lua_getglobal(p->L, "on_event");
        if (!lua_isfunction(p->L, -1)) {
            lua_pop(p->L, 1);
            continue;
        }
        lua_pushstring(p->L, ev.type.c_str());
        lua_pushinteger(p->L, ev.entity);
        lua_pushinteger(p->L, ev.command);
        lua_pushstring(p->L, ev.message.c_str());
        lua_newtable(p->L);
        if (ev.collision) {
            auto &c = *ev.collision;
            auto number = [&](const char *key, double value) {
                lua_pushnumber(p->L, value);
                lua_setfield(p->L, -2, key);
            };
            lua_pushinteger(p->L, lua_Integer(c.other));
            lua_setfield(p->L, -2, "other");
            number("x", c.point.x);
            number("y", c.point.y);
            number("z", c.point.z);
            number("nx", c.normal.x);
            number("ny", c.normal.y);
            number("nz", c.normal.z);
            number("closing_speed", c.closingSpeed);
            number("energy", c.energy);
        }
        lua_sethook(p->L, Impl::hook, LUA_MASKCOUNT, 200000);
        if (lua_pcall(p->L, 5, 0, 0) != LUA_OK) {
            engine.emit("script_error", 0, lua_tostring(p->L, -1));
            lua_pop(p->L, 1);
            p->disabled = true;
            lua_sethook(p->L, nullptr, 0, 0);
            return;
        }
        lua_sethook(p->L, nullptr, 0, 0);
    }
    lua_getglobal(p->L, "update");
    if (!lua_isfunction(p->L, -1)) {
        lua_pop(p->L, 1);
        return;
    }
    lua_pushnumber(p->L, dt);
    lua_sethook(p->L, Impl::hook, LUA_MASKCOUNT, 200000);
    if (lua_pcall(p->L, 1, 0, 0) != LUA_OK) {
        engine.emit("script_error", 0, lua_tostring(p->L, -1));
        lua_pop(p->L, 1);
        p->disabled = true;
    }
    lua_sethook(p->L, nullptr, 0, 0);
}
void Scripts::input(const std::string &action, float value) {
    p->inputs[action] = value;
}
} // namespace aspera
