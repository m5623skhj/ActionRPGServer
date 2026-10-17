#pragma once

#include "PlayerSkillCatalog.h"

#include <cstdint>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>

namespace ActionRPG::PlayerSkills
{
    inline constexpr std::uint32_t MAX_CHARACTER_LEVEL = 1000000;

    inline std::uint64_t ExperienceValue(const Catalog::Json& inValue)
    {
        Catalog::Require(inValue.is_string(), "Experience must be a decimal string.");
        const auto text = inValue.get<std::string>();
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        Catalog::Require(!text.empty() && (text.size() == 1 || text.front() != '0')
            && text.front() >= '0' && text.front() <= '9' && error == std::errc{}
            && end == text.data() + text.size(), "Invalid experience value.");
        return value;
    }

    struct CharacterProgression
    {
        std::uint32_t level{};
        std::uint32_t skillPoints{};
        std::unordered_map<std::string, std::uint32_t> skillLevels;
        std::uint64_t experience{};

        [[nodiscard]] std::uint32_t GetSkillLevel(const std::string& inSkillId) const
        {
            const auto found = skillLevels.find(inSkillId);
            return found == skillLevels.end() ? 0 : found->second;
        }

        [[nodiscard]] Catalog::Json ToJson() const
        {
            return { { "level", level }, { "skillPoints", skillPoints }, { "skillLevels", skillLevels },
                { "experience", std::to_string(experience) } };
        }

        [[nodiscard]] static CharacterProgression Parse(const Catalog::Json& inValue)
        {
            Catalog::Keys(inValue, { "level", "skillPoints", "skillLevels", "experience" });
            CharacterProgression result;
            result.level = Catalog::Integer(inValue.at("level"), 1, MAX_CHARACTER_LEVEL);
            result.experience = ExperienceValue(inValue.at("experience"));
            Catalog::Require(result.level != MAX_CHARACTER_LEVEL || result.experience == 0, "Invalid capped experience.");
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
        std::uint64_t experienceBase{}, experiencePerLevel{};

        [[nodiscard]] CharacterProgression Create() const
        {
            return CharacterProgression{ initialLevel, initialSkillPoints, {} };
        }

        /** Server-only level advancement; award once per gained level, with no partial update on overflow. */
        [[nodiscard]] bool AdvanceLevel(CharacterProgression& inProgression, std::uint32_t inLevel) const
        {
            if (inLevel <= inProgression.level || inLevel > MAX_CHARACTER_LEVEL) return false;
            const auto reward = static_cast<std::uint64_t>(inLevel - inProgression.level) * skillPointsPerLevel;
            const auto points = static_cast<std::uint64_t>(inProgression.skillPoints) + reward;
            if (points > std::numeric_limits<std::uint32_t>::max()) return false;
            inProgression.level = inLevel;
            inProgression.skillPoints = static_cast<std::uint32_t>(points);
            if (inLevel == MAX_CHARACTER_LEVEL) inProgression.experience = 0;
            return true;
        }

        [[nodiscard]] std::uint64_t RequiredExperience(std::uint32_t inLevel) const
        {
            Catalog::Require(inLevel >= 1 && inLevel <= MAX_CHARACTER_LEVEL, "Invalid experience level.");
            return inLevel == MAX_CHARACTER_LEVEL ? 0 : experienceBase + (inLevel - 1) * experiencePerLevel;
        }

        [[nodiscard]] bool Normalized(const CharacterProgression& inProgression) const
        {
            return inProgression.level == MAX_CHARACTER_LEVEL ? inProgression.experience == 0
                : inProgression.experience < RequiredExperience(inProgression.level);
        }

        /** Compute multi-level gains in bounded logarithmic time. Costs above the available
         * U64 balance are treated as unaffordable without overflowing or partially mutating state.
         */
        [[nodiscard]] bool AddExperience(CharacterProgression& inProgression, std::uint64_t inAmount) const
        {
            if (!Normalized(inProgression)) return false;
            if (inProgression.level == MAX_CHARACTER_LEVEL) return true;
            if (inAmount > std::numeric_limits<std::uint64_t>::max() - inProgression.experience) return false;
            const auto available = inProgression.experience + inAmount;
            const auto first = RequiredExperience(inProgression.level);
            const auto cost = [&](std::uint32_t inCount, std::uint64_t& outCost)
            {
                outCost = 0;
                if (inCount == 0) return true;
                if (first > available / inCount) return false;
                outCost = first * inCount;
                const auto triangle = static_cast<std::uint64_t>(inCount) * (inCount - 1) / 2;
                if (triangle && experiencePerLevel > (available - outCost) / triangle) return false;
                outCost += triangle * experiencePerLevel;
                return true;
            };
            std::uint32_t low{}, high = MAX_CHARACTER_LEVEL - inProgression.level;
            while (low < high)
            {
                const auto middle = low + (high - low + 1) / 2;
                std::uint64_t ignored{};
                if (cost(middle, ignored)) low = middle; else high = middle - 1;
            }
            const auto points = static_cast<std::uint64_t>(inProgression.skillPoints)
                + static_cast<std::uint64_t>(low) * skillPointsPerLevel;
            if (points > std::numeric_limits<std::uint32_t>::max()) return false;
            std::uint64_t spent{};
            if (!cost(low, spent)) return false;
            inProgression.level += low;
            inProgression.skillPoints = static_cast<std::uint32_t>(points);
            inProgression.experience = inProgression.level == MAX_CHARACTER_LEVEL ? 0 : available - spent;
            return true;
        }

        [[nodiscard]] static ProgressionPolicy Load(const std::filesystem::path& inPath)
        {
            Catalog::Require(std::filesystem::is_regular_file(inPath)
                && std::filesystem::file_size(inPath) <= 4096, "Missing or oversized character progression policy.");
            std::ifstream input(inPath, std::ios::binary);
            Catalog::Require(input.good(), "Unable to read character progression policy.");
            const auto value = Catalog::Json::parse(input);
            Catalog::Keys(value, { "format", "schemaVersion", "initialLevel", "initialSkillPoints", "skillPointsPerLevel",
                "experienceBase", "experiencePerLevel" });
            Catalog::Require(value.at("format") == "CharacterProgression" && value.at("schemaVersion") == 2,
                "Unsupported character progression policy.");
            ProgressionPolicy result{ Catalog::Integer(value.at("initialLevel"), 1, MAX_CHARACTER_LEVEL),
                Catalog::Integer(value.at("initialSkillPoints"), 0, std::numeric_limits<std::uint32_t>::max()),
                Catalog::Integer(value.at("skillPointsPerLevel"), 0, 1000000),
                ExperienceValue(value.at("experienceBase")), ExperienceValue(value.at("experiencePerLevel")) };
            Catalog::Require(result.experienceBase > 0 && result.experiencePerLevel <=
                (std::numeric_limits<std::uint64_t>::max() - result.experienceBase) / (MAX_CHARACTER_LEVEL - 2),
                "Experience curve exceeds U64 range.");
            return result;
        }
    };
}
