#include "DungeonCatalog.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace TownServer::Domain
{
    DungeonCatalog DungeonCatalog::Load(const std::filesystem::path& inPath)
    {
        std::ifstream stream(inPath);
        if (!stream)
        {
            throw std::runtime_error("Unable to open dungeon catalog: " + inPath.string());
        }

        nlohmann::json document;
        stream >> document;
        if (document.at("version").get<int>() != 1)
        {
            throw std::runtime_error("Unsupported dungeon catalog version.");
        }

        DungeonCatalog result;
        for (const nlohmann::json& inputGroup : document.at("groups"))
        {
            const std::string groupId = inputGroup.at("id").get<std::string>();
            if (groupId.empty() || groupId.size() > 64 || result.groups.contains(groupId))
            {
                throw std::runtime_error("Invalid or duplicate dungeon group id.");
            }

            std::vector<TownProtocol::DungeonOption> options;
            std::unordered_set<std::uint32_t> dungeonIds;
            for (const nlohmann::json& inputDungeon : inputGroup.at("dungeons"))
            {
                TownProtocol::DungeonOption option{
                    inputDungeon.at("id").get<std::uint32_t>(),
                    inputDungeon.at("name").get<std::string>(),
                    inputDungeon.at("levelRange").get<std::string>(),
                    inputDungeon.at("description").get<std::string>()
                };
                if (option.dungeonId == 0 || !dungeonIds.insert(option.dungeonId).second
                    || option.name.empty() || option.name.size() > 96
                    || option.levelRange.size() > 48 || option.description.size() > 256)
                {
                    throw std::runtime_error("Invalid or duplicate dungeon option.");
                }
                options.push_back(std::move(option));
            }
            if (options.empty() || options.size() > 64)
            {
                throw std::runtime_error("Dungeon group must contain between 1 and 64 dungeons.");
            }
            result.groups.emplace(groupId, std::move(options));
        }
        return result;
    }

    const std::vector<TownProtocol::DungeonOption>* DungeonCatalog::FindGroup(
        const std::string_view inGroupId) const
    {
        const auto iterator = groups.find(std::string(inGroupId));
        return iterator == groups.end() ? nullptr : &iterator->second;
    }

    bool DungeonCatalog::Contains(const std::string_view inGroupId, const std::uint32_t inDungeonId) const
    {
        const std::vector<TownProtocol::DungeonOption>* options = FindGroup(inGroupId);
        if (options == nullptr)
        {
            return false;
        }
        for (const TownProtocol::DungeonOption& option : *options)
        {
            if (option.dungeonId == inDungeonId)
            {
                return true;
            }
        }
        return false;
    }
}
