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
#include <vector>

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
            Require(inValue.is_number(), "Expected a dungeon number.");
            const float value = inValue.get<float>();
            Require(std::isfinite(value) && std::abs(value) <= 1000000.0f, "Invalid dungeon coordinate.");
            return value;
        }
        std::uint32_t PositiveInteger(const nlohmann::json& inValue, const std::uint32_t inMax)
        {
            Require(inValue.is_number_integer() && inValue > 0 && inValue <= inMax, "Invalid positive integer.");
            return inValue.get<std::uint32_t>();
        }
        int GridCoordinate(const nlohmann::json& inValue)
        {
            Require(inValue.is_number_integer() && inValue >= -1000 && inValue <= 1000,
                "Minimap coordinates must be integers between -1000 and 1000.");
            return inValue.get<int>();
        }
        void ValidateName(const nlohmann::json& inValue)
        {
            const std::string value = inValue.get<std::string>();
            Require(value.size() <= 1024 && value.find_first_not_of(" \t\r\n") != std::string::npos, "Invalid name.");
        }
        void ValidatePolygon(const nlohmann::json& inPolygon)
        {
            Require(inPolygon.is_array() && inPolygon.size() >= 3 && inPolygon.size() <= 64,
                "Invalid dungeon polygon.");
            for (const auto& point : inPolygon) (void)DungeonDefinition::Point(point);
        }
        void ValidateMap(const nlohmann::json& inMap, const MonsterDefinition::Catalog& inMonsters)
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
                const auto dataId = PositiveInteger(monster.at("dataId"), 1000000);
                Require(inMonsters.contains(dataId), "Unknown monster Data ID.");
                Require(monster.at("facingLeft").is_boolean(), "Monster facingLeft must be boolean.");
                const DungeonPoint position = DungeonDefinition::Point(monster.at("position"));
                Require(DungeonDefinition::Movable(inMap, position), "Monster placement is not walkable.");
                for (const auto& zone : zones)
                    Require(!DungeonDefinition::Contains(zone.at("polygon"), position), "Monster is inside a gate.");
            }
        }

        std::string ConnectionSide(const nlohmann::json& inFrom, const nlohmann::json& inTo)
        {
            const int dx = GridCoordinate(inTo.at("layout").at("x")) - GridCoordinate(inFrom.at("layout").at("x"));
            const int dy = GridCoordinate(inTo.at("layout").at("y")) - GridCoordinate(inFrom.at("layout").at("y"));
            if (dx == 1 && dy == 0) return "east";
            if (dx == -1 && dy == 0) return "west";
            if (dx == 0 && dy == 1) return "south";
            if (dx == 0 && dy == -1) return "north";
            return {};
        }
        bool NearSide(const nlohmann::json& inMap, const DungeonPoint inPoint, const std::string& inSide)
        {
            const auto& world = inMap.at("world");
            const float left = Number(world.at("left")), right = Number(world.at("right"));
            const float top = Number(world.at("top")), bottom = Number(world.at("bottom"));
            if (inPoint.x < left || inPoint.x > right || inPoint.y < top || inPoint.y > bottom) return false;
            const float dx = std::min(128.0f, (right - left) * 0.2f), dy = std::min(128.0f, (bottom - top) * 0.2f);
            if (inSide == "east") return inPoint.x >= right - dx;
            if (inSide == "west") return inPoint.x <= left + dx;
            if (inSide == "north") return inPoint.y <= top + dy;
            if (inSide == "south") return inPoint.y >= bottom - dy;
            return false;
        }

        // Match room links to physical gates; bidirectional links require a gate in each room.
        void ValidateComposition(const nlohmann::json& inManifest, const nlohmann::json& inMaps)
        {
            using Json = nlohmann::json;
            std::unordered_map<std::string, const Json*> rooms, mapRooms, connections;
            std::unordered_set<std::string> grids, pairs, gateDirections;
            const std::string dungeonId = inManifest.at("dungeonId");
            Require(ValidId(dungeonId), "Invalid dungeon ID.");
            ValidateName(inManifest.at("name"));
            for (const auto& room : inManifest.at("rooms"))
            {
                const std::string id = room.at("id"), mapId = room.at("mapId");
                Require(ValidId(id) && rooms.emplace(id, &room).second, "Duplicate or invalid room ID.");
                Require(mapId == dungeonId + "_" + id, "Map ID does not match dungeon and room IDs.");
                Require(mapRooms.emplace(mapId, &room).second, "Duplicate map ID.");
                ValidateName(room.at("name"));
                Require(room.at("kind") == "normal" || room.at("kind") == "boss", "Unknown room kind.");
                const int x = GridCoordinate(room.at("layout").at("x")), y = GridCoordinate(room.at("layout").at("y"));
                Require(grids.insert(std::to_string(x) + "," + std::to_string(y)).second, "Overlapping minimap rooms.");
            }
            const std::string entry = inManifest.at("entryRoomId");
            Require(rooms.contains(entry) && rooms.at(entry)->at("mapId") == inManifest.at("entryMapId"),
                "Unknown or mismatched entry room.");
            const auto& links = inManifest.at("connections");
            Require(links.is_array() && links.size() <= 1024, "Invalid dungeon connection list.");
            for (const auto& link : links)
            {
                const std::string id = link.at("id"), from = link.at("fromRoomId"), to = link.at("toRoomId");
                Require(ValidId(id) && connections.emplace(id, &link).second, "Duplicate or invalid connection ID.");
                Require(rooms.contains(from) && rooms.contains(to) && from != to, "Invalid connection endpoints.");
                Require(link.at("bidirectional").is_boolean(), "Connection bidirectional must be boolean.");
                Require(!ConnectionSide(*rooms.at(from), *rooms.at(to)).empty(), "Connected rooms must be adjacent on the minimap.");
                const std::string pair = std::min(from, to) + "|" + std::max(from, to);
                Require(pairs.insert(pair).second, "Duplicate room connection.");
            }
            std::unordered_map<std::string, std::vector<std::string>> destinations;
            for (const auto& [mapId, map] : inMaps.items())
            {
                const auto& room = *mapRooms.at(mapId);
                const std::string roomId = room.at("id");
                for (const auto& zone : map.at("transitionZones"))
                {
                    const std::string connectionId = zone.at("connectionId");
                    Require(connections.contains(connectionId), "Gate references an unknown connection.");
                    const auto& link = *connections.at(connectionId);
                    const auto& action = zone.at("action");
                    const std::string targetMapId = action.at("targetMapId");
                    Require(inMaps.contains(targetMapId), "Unknown gate destination.");
                    const auto& targetRoom = *mapRooms.at(targetMapId);
                    const std::string targetRoomId = targetRoom.at("id");
                    Require((link.at("fromRoomId") == roomId && link.at("toRoomId") == targetRoomId)
                        || (link.at("bidirectional").get<bool>() && link.at("toRoomId") == roomId
                            && link.at("fromRoomId") == targetRoomId), "Gate destination does not match connection direction.");
                    const std::string side = ConnectionSide(room, targetRoom);
                    Require(!side.empty() && zone.at("direction") == side, "Gate direction does not match minimap.");
                    for (const auto& point : zone.at("polygon"))
                        Require(NearSide(map, DungeonDefinition::Point(point), side), "Gate is outside its exit boundary.");
                    const auto& targetMap = inMaps.at(targetMapId);
                    const auto& entries = targetMap.at("entryPoints");
                    const auto arrival = std::find_if(entries.begin(), entries.end(), [&action](const Json& value)
                        { return value.at("id") == action.at("targetEntryPointId"); });
                    Require(arrival != entries.end(), "Unknown destination entry point.");
                    const std::string opposite = side == "east" ? "west" : side == "west" ? "east"
                        : side == "north" ? "south" : "north";
                    Require(NearSide(targetMap, DungeonDefinition::Point(arrival->at("position")), opposite),
                        "Destination entry is not on the opposite boundary.");
                    gateDirections.insert(connectionId + "|" + roomId);
                    destinations[roomId].push_back(targetRoomId);
                }
            }
            for (const auto& [id, link] : connections)
            {
                Require(gateDirections.contains(id + "|" + link->at("fromRoomId").get<std::string>()), "Connection lacks an outbound gate.");
                if (link->at("bidirectional").get<bool>())
                    Require(gateDirections.contains(id + "|" + link->at("toRoomId").get<std::string>()), "Connection lacks a return gate.");
            }
            std::unordered_set<std::string> reached{ entry };
            std::vector<std::string> queue{ entry };
            for (std::size_t index = 0; index < queue.size(); ++index)
                for (const auto& next : destinations[queue[index]])
                    if (reached.insert(next).second) queue.push_back(next);
            Require(reached.size() == rooms.size(), "A dungeon room cannot be reached from the entry.");
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
        DungeonDefinition::LoadDirectory(const std::filesystem::path& inDirectory, const MonsterDefinition::Catalog& inMonsters)
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
                definition->dataId = PositiveInteger(manifest.at("dataId"), 1000000);
                definition->maxPlayers = PositiveInteger(manifest.at("maxPlayers"), 32);
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
                    ValidateMap(map, inMonsters);
                    for (const auto& monster : map.at("monsters"))
                    {
                        const auto dataId = monster.at("dataId").get<std::uint32_t>();
                        definition->monsterDefinitions.emplace(dataId, inMonsters.at(dataId));
                    }
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
                ValidateComposition(manifest, world.at("maps"));
                definition->configuration = manifest;
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
