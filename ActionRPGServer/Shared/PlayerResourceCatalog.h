#pragma once

#include "PlayerSkillCatalog.h"
#include <fstream>
#include <optional>

namespace ActionRPG::Combat
{
    struct PlayerResourceLimits
    {
        std::uint32_t maxHp{};
        std::optional<std::uint32_t> maxMp;
    };

    /// Reads the resource subset of the single server Combat.json definition for Town and Room.
    class PlayerResourceCatalog final
    {
    public:
        using Json = PlayerSkills::Catalog::Json;
        PlayerResourceLimits defaults;
        std::unordered_map<std::uint32_t, PlayerResourceLimits> characters;

        [[nodiscard]] const PlayerResourceLimits& Get(std::uint32_t inCharacterId) const
        { return characters.at(inCharacterId); }

        [[nodiscard]] static PlayerResourceCatalog Parse(const Json& inPlayer)
        {
            using Rules = PlayerSkills::Catalog;
            Rules::Require(inPlayer.is_object(), "Invalid player resource definition.");
            PlayerResourceCatalog result;
            result.defaults.maxHp = Rules::Integer(inPlayer.at("maxHp"), 1, 1000000);
            if (inPlayer.contains("maxMp")) result.defaults.maxMp = Rules::Integer(inPlayer.at("maxMp"), 1, 1000000);
            const auto& characters = inPlayer.at("characters");
            Rules::Require(characters.is_array() && !characters.empty() && characters.size() <= 256,
                "Invalid resource character list.");
            for (const auto& character : characters)
            {
                Rules::Require(character.is_object(), "Invalid resource character definition.");
                const auto id = Rules::Integer(character.at("characterId"), 1, 1000000);
                auto limits = result.defaults;
                if (character.contains("maxMp")) limits.maxMp = Rules::Integer(character.at("maxMp"), 1, 1000000);
                Rules::Require(result.characters.emplace(id, limits).second, "Duplicate resource character.");
            }
            return result;
        }

        [[nodiscard]] static PlayerResourceCatalog Load(const std::filesystem::path& inPath)
        {
            using Rules = PlayerSkills::Catalog;
            Rules::Require(std::filesystem::is_regular_file(inPath) && std::filesystem::file_size(inPath) <= 1024 * 1024,
                "Missing or oversized combat resource definition.");
            std::ifstream input(inPath, std::ios::binary);
            Rules::Require(input.good(), "Cannot read combat resource definition.");
            const auto source = Json::parse(input);
            Rules::Require(source.is_object() && source.at("version") == 3, "Unsupported combat resource version.");
            return Parse(source.at("player"));
        }
    };
}
