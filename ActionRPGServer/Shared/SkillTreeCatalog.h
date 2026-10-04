#pragma once

#include "CharacterProgression.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace ActionRPG::PlayerSkills
{
    class SkillTreeCatalog final
    {
    public:
        Catalog::Json source;
        std::unordered_map<std::string, Catalog::Json> nodes;
        std::uint32_t skillLevelInterval{};

        [[nodiscard]] std::uint32_t RequiredLevel(const std::string& inSkillId, std::uint32_t inSkillLevel) const
        {
            const auto required = nodes.at(inSkillId).at("requiredLevel").get<std::uint64_t>()
                + static_cast<std::uint64_t>(inSkillLevel - 1) * skillLevelInterval;
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(required,
                std::numeric_limits<std::uint32_t>::max()));
        }

        [[nodiscard]] std::uint32_t Damage(const Catalog& inCatalog, const std::string& inSkillId,
            std::uint32_t inSkillLevel) const
        {
            const auto& execution = inCatalog.skills.at(inSkillId).at("execution");
            if (!execution.contains("damage") || inSkillLevel == 0) return 0;
            const auto damage = execution.at("damage").get<std::uint64_t>()
                + static_cast<std::uint64_t>(inSkillLevel - 1) * nodes.at(inSkillId).at("damagePerLevel").get<std::uint32_t>();
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(damage,
                std::numeric_limits<std::uint32_t>::max()));
        }

        [[nodiscard]] std::string CanLearn(const Catalog& inCatalog, const CharacterProgression& inProgression,
            std::uint32_t inCharacterId, const std::string& inSkillId, std::uint32_t inExpectedLevel) const
        {
            const auto found = nodes.find(inSkillId);
            if (found == nodes.end()) return "UnknownSkill";
            const auto& node = found->second;
            const auto& skill = inCatalog.skills.at(inSkillId);
            if (inCatalog.characterIds.at(skill.at("characterId").get<std::string>()) != inCharacterId)
                return "WrongCharacter";
            const auto current = inProgression.GetSkillLevel(inSkillId);
            if (current != inExpectedLevel) return "StaleSkillLevel";
            if (current >= 1000000 || (!node.at("maxSkillLevel").is_null()
                && current >= node.at("maxSkillLevel").get<std::uint32_t>())) return "MaximumSkillLevel";
            if (inProgression.level < RequiredLevel(inSkillId, current + 1)) return "RequiredLevel";
            for (const auto& prerequisite : node.at("prerequisites"))
                if (inProgression.GetSkillLevel(prerequisite.at("skillId").get<std::string>())
                    < prerequisite.at("skillLevel").get<std::uint32_t>()) return "Prerequisite";
            if (inProgression.skillPoints < node.at("spCost").get<std::uint32_t>()) return "InsufficientSp";
            return "Succeeded";
        }

        // Call on the progression owner's strand. Expected rank prevents retrying one purchase as another rank.
        [[nodiscard]] std::string Learn(const Catalog& inCatalog, CharacterProgression& inProgression,
            std::uint32_t inCharacterId, const std::string& inSkillId, std::uint32_t inExpectedLevel) const
        {
            const auto result = CanLearn(inCatalog, inProgression, inCharacterId, inSkillId, inExpectedLevel);
            if (result != "Succeeded") return result;
            inProgression.skillLevels[inSkillId] = inExpectedLevel + 1;
            inProgression.skillPoints -= nodes.at(inSkillId).at("spCost").get<std::uint32_t>();
            return result;
        }

        void ValidateProgression(const Catalog& inCatalog, const CharacterProgression& inProgression,
            std::uint32_t inCharacterId) const
        {
            Catalog::Require(inCatalog.characterIds.contains("Character" + std::to_string(inCharacterId)),
                "Unknown progression character.");
            for (const auto& [id, level] : inProgression.skillLevels)
            {
                Catalog::Require(nodes.contains(id), "Learned skill missing from tree.");
                const auto& node = nodes.at(id);
                Catalog::Require(inCatalog.characterIds.at(inCatalog.skills.at(id).at("characterId").get<std::string>())
                    == inCharacterId && inProgression.level >= RequiredLevel(id, level), "Invalid learned skill owner/level.");
                Catalog::Require(node.at("maxSkillLevel").is_null()
                    || level <= node.at("maxSkillLevel").get<std::uint32_t>(), "Learned skill above maximum.");
                for (const auto& prerequisite : node.at("prerequisites"))
                    Catalog::Require(inProgression.GetSkillLevel(prerequisite.at("skillId").get<std::string>())
                        >= prerequisite.at("skillLevel").get<std::uint32_t>(), "Unmet learned prerequisite.");
            }
        }

        [[nodiscard]] static SkillTreeCatalog Parse(Catalog::Json inSource, const Catalog& inCatalog)
        {
            Catalog::Keys(inSource, { "format", "schemaVersion", "skillLevelInterval", "nodes" });
            Catalog::Require(inSource.at("format") == "SkillTrees" && inSource.at("schemaVersion") == 1,
                "Unsupported skill tree contract.");
            SkillTreeCatalog result;
            result.skillLevelInterval = Catalog::Integer(inSource.at("skillLevelInterval"), 1, 1000000);
            Catalog::Require(inSource.at("nodes").is_array() && inSource.at("nodes").size() <= 256,
                "Invalid skill tree node count.");
            std::unordered_set<std::string> positions;
            for (const auto& node : inSource.at("nodes"))
            {
                Catalog::Keys(node, { "skillId", "requiredLevel", "spCost", "maxSkillLevel", "prerequisites",
                    "description", "icon", "column", "row", "damagePerLevel" });
                const auto id = node.at("skillId").get<std::string>();
                Catalog::Require(inCatalog.skills.contains(id) && result.nodes.emplace(id, node).second,
                    "Unknown/duplicate tree skill.");
                Catalog::Integer(node.at("requiredLevel"), 1, 1000000);
                Catalog::Integer(node.at("spCost"), 1, 1000000);
                if (!node.at("maxSkillLevel").is_null()) Catalog::Integer(node.at("maxSkillLevel"), 1, 1000000);
                Catalog::Integer(node.at("damagePerLevel"), 0, 1000000);
                const auto column = Catalog::Integer(node.at("column"), 0, 255);
                const auto row = Catalog::Integer(node.at("row"), 0, 255);
                const auto character = inCatalog.skills.at(id).at("characterId").get<std::string>();
                Catalog::Require(positions.insert(character + ":" + std::to_string(column) + ":" + std::to_string(row)).second,
                    "Overlapping skill tree nodes.");
                const auto description = node.at("description").get<std::string>();
                Catalog::Require(!description.empty() && description.size() <= 2048, "Invalid skill description.");
                const auto icon = node.at("icon").get<std::string>();
                Catalog::Require(icon.starts_with("Images/") && icon.size() <= 255 && icon.ends_with(".png")
                    && icon.find("..") == std::string::npos && icon.find('\\') == std::string::npos
                    && icon.find(':') == std::string::npos, "Invalid skill icon path.");
                Catalog::Require(node.at("prerequisites").is_array() && node.at("prerequisites").size() <= 256,
                    "Invalid prerequisite count.");
            }
            Catalog::Require(result.nodes.size() == inCatalog.skills.size(), "Every skill needs a tree node.");
            std::unordered_map<std::string, std::uint8_t> visits;
            // DFS rejects prerequisite cycles before runtime; each edge is checked for owner and rank validity.
            std::function<void(const std::string&)> visit = [&](const std::string& id)
            {
                Catalog::Require(visits[id] != 1, "Skill prerequisite cycle.");
                if (visits[id] == 2) return;
                visits[id] = 1;
                std::unordered_set<std::string> references;
                for (const auto& prerequisite : result.nodes.at(id).at("prerequisites"))
                {
                    Catalog::Keys(prerequisite, { "skillId", "skillLevel" });
                    const auto target = prerequisite.at("skillId").get<std::string>();
                    const auto rank = Catalog::Integer(prerequisite.at("skillLevel"), 1, 1000000);
                    Catalog::Require(target != id && result.nodes.contains(target) && references.insert(target).second,
                        "Invalid/duplicate prerequisite reference.");
                    Catalog::Require(inCatalog.skills.at(target).at("characterId") == inCatalog.skills.at(id).at("characterId"),
                        "Prerequisite belongs to another character.");
                    const auto& maximum = result.nodes.at(target).at("maxSkillLevel");
                    Catalog::Require(maximum.is_null() || rank <= maximum.get<std::uint32_t>(), "Unreachable prerequisite rank.");
                    visit(target);
                }
                visits[id] = 2;
            };
            for (const auto& [id, node] : result.nodes) visit(id);
            result.source = std::move(inSource);
            return result;
        }

        [[nodiscard]] static SkillTreeCatalog Load(const std::filesystem::path& inPath, const Catalog& inCatalog)
        {
            Catalog::Require(std::filesystem::is_regular_file(inPath)
                && std::filesystem::file_size(inPath) <= 1024 * 1024, "Missing/oversized skill tree file.");
            std::ifstream input(inPath, std::ios::binary);
            Catalog::Require(input.good(), "Cannot read skill tree file.");
            return Parse(Catalog::Json::parse(input), inCatalog);
        }
    };
}
