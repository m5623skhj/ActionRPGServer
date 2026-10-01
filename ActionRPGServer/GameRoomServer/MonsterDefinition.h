#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameRoomServer
{
    // Definitions are loaded before networking starts and shared as immutable objects.
    struct MonsterDefinition
    {
        using Catalog = std::unordered_map<std::uint32_t, std::shared_ptr<const MonsterDefinition>>;

        std::uint32_t dataId{};
        std::string id;
        std::string name;
        std::uint32_t maxHp{};

        [[nodiscard]] const nlohmann::json& GetAi() const;
        [[nodiscard]] const nlohmann::json& GetSkills() const;
        [[nodiscard]] const nlohmann::json& GetMotions() const;
        [[nodiscard]] static Catalog LoadCatalog(const std::filesystem::path& inPath);

    private:
        std::shared_ptr<const nlohmann::json> document;
        std::size_t monsterIndex{};
    };
}
