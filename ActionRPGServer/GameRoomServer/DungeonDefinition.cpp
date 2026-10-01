#include "DungeonDefinition.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace GameRoomServer
{
    namespace
    {
        constexpr std::uintmax_t MAX_WORLD_BYTES = 4 * 1024 * 1024;
        void Require(const bool inCondition, const char* inMessage)
        {
            if (!inCondition) throw std::runtime_error(inMessage);
        }
        bool ValidId(const std::string& inId)
        {
            static const std::regex pattern("[A-Za-z][A-Za-z0-9_-]{0,63}");
            return std::regex_match(inId, pattern);
        }
        nlohmann::json ReadJson(const std::filesystem::path& inPath)
        {
            Require(std::filesystem::file_size(inPath) <= MAX_WORLD_BYTES, "Dungeon JSON exceeds 4 MB.");
            std::ifstream input(inPath, std::ios::binary);
            Require(input.good(), "Cannot read dungeon JSON.");
            return nlohmann::json::parse(input);
        }
        float Number(const nlohmann::json& inValue)
        {
            const float value = inValue.get<float>();
            Require(std::isfinite(value) && std::abs(value) <= 1000000.0f, "Invalid dungeon coordinate.");
            return value;
        }
        void ValidatePolygon(const nlohmann::json& inPolygon)
        {
            Require(inPolygon.is_array() && inPolygon.size() >= 3 && inPolygon.size() <= 64,
                "Invalid dungeon polygon.");
            for (const auto& point : inPolygon) (void)DungeonDefinition::Point(point);
        }
        void ValidateMap(const nlohmann::json& inMap)
        {
            Require(inMap.at("version") == 5 && inMap.at("format") == "DungeonRoom", "Unsupported dungeon map version.");
            const auto& world = inMap.at("world");
            const float left = Number(world.at("left")), top = Number(world.at("top"));
            const float right = Number(world.at("right")), bottom = Number(world.at("bottom"));
            Require(right > left && bottom > top && right - left <= 100000 && bottom - top <= 100000,
                "Invalid world bounds.");
            Require(Number(inMap.at("sectorWidth")) > 0 && Number(inMap.at("sectorHeight")) > 0, "Invalid sector size.");
            for (const char* key : { "walkablePolygons", "blockedPolygons" })
            {
                const auto& polygons = inMap.at(key);
                Require(polygons.is_array() && polygons.size() <= 256, "Too many polygons.");
                for (const auto& polygon : polygons) ValidatePolygon(polygon);
            }
            Require(!inMap.at("walkablePolygons").empty(), "No walkable terrain.");
            const auto& images = inMap.at("images");
            Require(images.is_array() && images.size() <= 576, "Too many map images.");
            static const std::regex assetPattern("Images/Dungeons/[A-Za-z0-9_-]+\\.(png|jpg|webp|bmp)");
            for (const auto& image : images)
            {
                Require(std::regex_match(image.at("asset").get<std::string>(), assetPattern), "Invalid asset path.");
                (void)Number(image.at("x")); (void)Number(image.at("y"));
                Require(Number(image.at("width")) > 0 && Number(image.at("height")) > 0, "Invalid image size.");
            }
            std::unordered_set<std::string> ids;
            const auto& entries = inMap.at("entryPoints");
            Require(entries.is_array() && !entries.empty() && entries.size() <= 256, "Invalid arrival points.");
            for (const auto& entry : entries)
            {
                const std::string id = entry.at("id");
                Require(ValidId(id) && ids.insert(id).second, "Duplicate arrival point.");
                Require(DungeonDefinition::Movable(inMap, DungeonDefinition::Point(entry.at("position"))),
                    "Arrival point is not walkable.");
            }
            const auto& zones = inMap.at("transitionZones");
            Require(zones.is_array() && zones.size() <= 256, "Too many gates.");
            ids.clear();
            for (const auto& zone : zones)
            {
                const std::string id = zone.at("id");
                Require(ValidId(id) && ids.insert(id).second, "Duplicate gate.");
                ValidatePolygon(zone.at("polygon"));
                Require(zone.at("action").at("type") == "MapTransfer", "Unsupported dungeon gate action.");
            }
            const auto& monsters = inMap.at("monsters");
            Require(monsters.is_array() && monsters.size() <= 256, "Too many monsters.");
            ids.clear();
            for (const auto& monster : monsters)
            {
                const std::string id = monster.at("id");
                Require(ValidId(id) && ids.insert(id).second, "Duplicate monster placement.");
                Require(monster.at("dataId") == 1 && monster.at("facingLeft").is_boolean(), "Unknown monster Data ID.");
                const DungeonPoint position = DungeonDefinition::Point(monster.at("position"));
                Require(DungeonDefinition::Movable(inMap, position), "Monster placement is not walkable.");
                for (const auto& zone : zones)
                    Require(!DungeonDefinition::Contains(zone.at("polygon"), position), "Monster is inside a gate.");
            }
        }
    }

    DungeonPoint DungeonDefinition::Point(const nlohmann::json& inValue)
    {
        return { Number(inValue.at("x")), Number(inValue.at("y")) };
    }

    bool DungeonDefinition::Contains(const nlohmann::json& inPolygon, const DungeonPoint inPoint)
    {
        bool inside = false;
        if (inPolygon.empty()) return false;
        for (std::size_t index = 0, previous = inPolygon.size() - 1; index < inPolygon.size(); previous = index++)
        {
            const DungeonPoint a = Point(inPolygon[previous]), b = Point(inPolygon[index]);
            const float cross = (b.x - a.x) * (inPoint.y - a.y) - (b.y - a.y) * (inPoint.x - a.x);
            if (std::abs(cross) < 0.001f && inPoint.x >= std::min(a.x, b.x) && inPoint.x <= std::max(a.x, b.x)
                && inPoint.y >= std::min(a.y, b.y) && inPoint.y <= std::max(a.y, b.y)) return true;
            if ((a.y > inPoint.y) != (b.y > inPoint.y)
                && inPoint.x < (b.x - a.x) * (inPoint.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside;
    }

    // Require the entire foot ellipse inside one walkable polygon and outside obstacles.
    bool DungeonDefinition::Movable(const nlohmann::json& inMap, const DungeonPoint inPoint)
    {
        const auto& world = inMap.at("world");
        if (inPoint.x - 32 < world.at("left").get<float>() || inPoint.x + 32 > world.at("right").get<float>()
            || inPoint.y - 18 < world.at("top").get<float>() || inPoint.y + 18 > world.at("bottom").get<float>()) return false;
        const auto edgeDistance = [inPoint](const nlohmann::json& polygon)
        {
            float distance = 100000000.0f;
            for (std::size_t index = 0; index < polygon.size(); ++index)
            {
                const DungeonPoint a = Point(polygon[index]), b = Point(polygon[(index + 1) % polygon.size()]);
                const float ax = (a.x - inPoint.x) / 32, ay = (a.y - inPoint.y) / 18;
                const float dx = (b.x - a.x) / 32, dy = (b.y - a.y) / 18;
                const float length = dx * dx + dy * dy;
                const float t = length > 0 ? std::clamp(-(ax * dx + ay * dy) / length, 0.0f, 1.0f) : 0;
                distance = std::min(distance, std::hypot(ax + t * dx, ay + t * dy));
            }
            return distance;
        };
        bool walkable = false;
        for (const auto& polygon : inMap.at("walkablePolygons"))
            if (Contains(polygon, inPoint) && edgeDistance(polygon) >= 0.99999f) { walkable = true; break; }
        if (!walkable) return false;
        for (const auto& polygon : inMap.at("blockedPolygons"))
            if (Contains(polygon, inPoint) || edgeDistance(polygon) <= 1.00001f) return false;
        return true;
    }

    std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>>
        DungeonDefinition::LoadDirectory(const std::filesystem::path& inDirectory)
    {
        std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>> definitions;
        if (!std::filesystem::exists(inDirectory)) return definitions;
        for (const auto& directory : std::filesystem::directory_iterator(inDirectory))
        {
            if (!directory.is_directory() || !std::filesystem::exists(directory.path() / "Dungeon.json")) continue;
            std::shared_ptr<DungeonDefinition> definition;
            try
            {
                const auto manifest = ReadJson(directory.path() / "Dungeon.json");
                Require(manifest.at("version") == 3, "Re-export this dungeon with monster placement support.");
                definition = std::make_shared<DungeonDefinition>();
                definition->dataId = manifest.at("dataId").get<std::uint32_t>();
                definition->maxPlayers = manifest.at("maxPlayers").get<std::uint32_t>();
                Require(definition->dataId > 0 && definition->dataId <= 1000000 && definition->maxPlayers > 0
                    && definition->maxPlayers <= 32, "Invalid dungeon Data ID or party size.");
                Require(manifest.at("rooms").is_array() && !manifest.at("rooms").empty()
                    && manifest.at("rooms").size() <= 256, "Invalid room list.");
                auto& world = definition->world;
                world = { {"version", 1}, {"dataId", definition->dataId}, {"entryMapId", manifest.at("entryMapId")},
                    {"maps", nlohmann::json::object()}, {"playerSpawns", nlohmann::json::array()} };
                std::size_t monsterCount = 0;
                for (const auto& room : manifest.at("rooms"))
                {
                    const std::string mapId = room.at("mapId");
                    Require(ValidId(mapId) && !world["maps"].contains(mapId), "Duplicate or invalid map ID.");
                    const std::string expectedPath = "Maps/" + mapId + ".json";
                    Require(room.at("mapPath") == expectedPath, "Invalid dungeon map path.");
                    auto map = ReadJson(directory.path() / expectedPath);
                    Require(map.at("mapId") == mapId, "Mismatched map ID.");
                    ValidateMap(map);
                    monsterCount += map.at("monsters").size();
                    if (manifest.at("entryMapId") == mapId)
                    {
                        const auto& spawns = room.at("playerSpawns");
                        Require(spawns.is_array() && spawns.size() >= definition->maxPlayers && spawns.size() <= 32,
                            "Not enough player spawns.");
                        for (const auto& spawn : spawns)
                        {
                            const DungeonPoint position = Point(spawn.at("position"));
                            Require(Movable(map, position), "Player spawn is not walkable.");
                            world["playerSpawns"].push_back(spawn.at("position"));
                        }
                    }
                    world["maps"][mapId] = std::move(map);
                }
                Require(monsterCount <= 4096 && !world["playerSpawns"].empty(), "Invalid dungeon spawn list.");
                for (const auto& map : world.at("maps"))
                    for (const auto& zone : map.at("transitionZones"))
                    {
                        const auto& action = zone.at("action");
                        const std::string target = action.at("targetMapId");
                        Require(world["maps"].contains(target), "Unknown gate destination.");
                        const auto& entries = world["maps"][target].at("entryPoints");
                        Require(std::any_of(entries.begin(), entries.end(), [&action](const auto& entry)
                            { return entry.at("id") == action.at("targetEntryPointId"); }), "Unknown destination entry point.");
                    }
                Require(world.dump().size() <= MAX_WORLD_BYTES - 512 * 1024, "Dungeon world exceeds streaming limit.");
            }
            catch (const std::exception& error)
            {
                std::cerr << "Dungeon skipped: " << directory.path() << ": " << error.what() << '\n';
                continue;
            }
            Require(definitions.emplace(definition->dataId, definition).second, "Duplicate dungeon Data ID.");
            std::cout << "Dungeon loaded: " << definition->dataId << " (" << directory.path() << ")\n";
        }
        return definitions;
    }
}
