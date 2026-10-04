#pragma once

#include "PlayerSkillCatalog.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>

namespace ActionRPG::PlayerSkills
{
    struct CharacterProgression
    {
        std::uint32_t level{};
        std::uint32_t skillPoints{};
        std::unordered_map<std::string, std::uint32_t> skillLevels;

        [[nodiscard]] std::uint32_t GetSkillLevel(const std::string& inSkillId) const
        {
            const auto found = skillLevels.find(inSkillId);
            return found == skillLevels.end() ? 0 : found->second;
        }

        [[nodiscard]] Catalog::Json ToJson() const
        {
            return { { "level", level }, { "skillPoints", skillPoints }, { "skillLevels", skillLevels } };
        }

        [[nodiscard]] static CharacterProgression Parse(const Catalog::Json& inValue)
        {
            Catalog::Keys(inValue, { "level", "skillPoints", "skillLevels" });
            CharacterProgression result;
            result.level = Catalog::Integer(inValue.at("level"), 1, 1000000);
            result.skillPoints = Catalog::Integer(inValue.at("skillPoints"), 0,
                std::numeric_limits<std::uint32_t>::max());
            const auto& skills = inValue.at("skillLevels");
            Catalog::Require(skills.is_object() && skills.size() <= 256, "Invalid learned skill count.");
            for (const auto& [id, rank] : skills.items())
            {
                Catalog::Require(Catalog::IsId(id), "Invalid learned skill ID.");
                result.skillLevels.emplace(id, Catalog::Integer(rank, 1, 1000000));
            }
            return result;
        }
    };

    // Immutable policy. Mutable progression belongs to the player's owning server strand.
    struct ProgressionPolicy
    {
        std::uint32_t initialLevel{};
        std::uint32_t initialSkillPoints{};
        std::uint32_t skillPointsPerLevel{};

        [[nodiscard]] CharacterProgression Create() const
        {
            return CharacterProgression{ initialLevel, initialSkillPoints, {} };
        }

        /** Server-only level advancement; award once per gained level, with no partial update on overflow. */
        [[nodiscard]] bool AdvanceLevel(CharacterProgression& inProgression, std::uint32_t inLevel) const
        {
            if (inLevel <= inProgression.level || inLevel > 1000000) return false;
            const auto reward = static_cast<std::uint64_t>(inLevel - inProgression.level) * skillPointsPerLevel;
            const auto points = static_cast<std::uint64_t>(inProgression.skillPoints) + reward;
            if (points > std::numeric_limits<std::uint32_t>::max()) return false;
            inProgression.level = inLevel;
            inProgression.skillPoints = static_cast<std::uint32_t>(points);
            return true;
        }

        [[nodiscard]] static ProgressionPolicy Load(const std::filesystem::path& inPath)
        {
            Catalog::Require(std::filesystem::is_regular_file(inPath)
                && std::filesystem::file_size(inPath) <= 4096, "Missing or oversized character progression policy.");
            std::ifstream input(inPath, std::ios::binary);
            Catalog::Require(input.good(), "Unable to read character progression policy.");
            const auto value = Catalog::Json::parse(input);
            Catalog::Keys(value, { "format", "schemaVersion", "initialLevel", "initialSkillPoints", "skillPointsPerLevel" });
            Catalog::Require(value.at("format") == "CharacterProgression" && value.at("schemaVersion") == 1,
                "Unsupported character progression policy.");
            return { Catalog::Integer(value.at("initialLevel"), 1, 1000000),
                Catalog::Integer(value.at("initialSkillPoints"), 0, std::numeric_limits<std::uint32_t>::max()),
                Catalog::Integer(value.at("skillPointsPerLevel"), 0, 1000000) };
        }
    };
}
