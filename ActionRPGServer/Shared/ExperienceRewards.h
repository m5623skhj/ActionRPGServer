#pragma once

#include "CharacterProgression.h"

namespace ActionRPG::PlayerSkills
{
    // Server-owned reward data is separate from editor exports. One immutable
    // version and award amount are captured in each durable Room fact.
    struct ExperienceRewards
    {
        struct DungeonReward { std::uint32_t recommendedLevel{}; std::uint64_t clearExperience{}; };
        std::uint32_t version{};
        std::string partyCredit;
        std::unordered_map<std::uint32_t, std::uint64_t> monsters;
        std::unordered_map<std::uint32_t, DungeonReward> dungeons;

        [[nodiscard]] static ExperienceRewards Load(const std::filesystem::path& inPath)
        {
            Catalog::Require(std::filesystem::is_regular_file(inPath)
                && std::filesystem::file_size(inPath) <= 16384, "Missing or oversized experience rewards.");
            std::ifstream input(inPath, std::ios::binary);
            Catalog::Require(input.good(), "Unable to read experience rewards.");
            const auto source = Catalog::Json::parse(input);
            Catalog::Keys(source, {"format", "schemaVersion", "version", "partyCredit", "monsters", "dungeons"});
            Catalog::Require(source.at("format") == "ExperienceRewards" && source.at("schemaVersion") == 1,
                "Unsupported experience rewards.");
            ExperienceRewards result;
            result.version = Catalog::Integer(source.at("version"), 1, 1000000);
            result.partyCredit = source.at("partyCredit").get<std::string>();
            Catalog::Require(result.partyCredit == "RoomParticipantsFull", "Unsupported experience credit policy.");
            for (const auto key : {"monsters", "dungeons"})
                Catalog::Require(source.at(key).is_array() && source.at(key).size() <= 256, "Invalid experience reward list.");
            for (const auto& row : source.at("monsters"))
            {
                Catalog::Keys(row, {"monsterDataId", "experience"});
                Catalog::Require(result.monsters.emplace(Catalog::Integer(row.at("monsterDataId"), 1, 1000000),
                    ExperienceValue(row.at("experience"))).second, "Duplicate monster experience reward.");
            }
            for (const auto& row : source.at("dungeons"))
            {
                Catalog::Keys(row, {"dungeonId", "recommendedLevel", "clearExperience"});
                Catalog::Require(result.dungeons.emplace(Catalog::Integer(row.at("dungeonId"), 1, 1000000),
                    DungeonReward{Catalog::Integer(row.at("recommendedLevel"), 1, MAX_CHARACTER_LEVEL),
                        ExperienceValue(row.at("clearExperience"))}).second, "Duplicate dungeon experience reward.");
            }
            return result;
        }
    };
}
