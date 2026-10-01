#pragma once

#include "MonsterDefinition.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace GameRoomServer
{
    struct DungeonPoint
    {
        float x{};
        float y{};
    };

    // Immutable authoring data; each GameRoom creates its own monster instances.
    struct DungeonDefinition
    {
        std::uint32_t dataId{};
        std::uint32_t maxPlayers{};
        nlohmann::json world;
        nlohmann::json configuration;
        MonsterDefinition::Catalog monsterDefinitions;

        [[nodiscard]] static std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>>
            LoadDirectory(const std::filesystem::path& inDirectory, const MonsterDefinition::Catalog& inMonsters);
        [[nodiscard]] static bool Contains(const nlohmann::json& inPolygon, DungeonPoint inPoint);
        [[nodiscard]] static bool Movable(const nlohmann::json& inMap, DungeonPoint inPoint);
        [[nodiscard]] static DungeonPoint Point(const nlohmann::json& inValue);
    };
}
