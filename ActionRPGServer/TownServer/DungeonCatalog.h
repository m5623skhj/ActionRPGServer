#pragma once

#include "Protocol.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TownServer::Domain
{
    class DungeonCatalog final
    {
    public:
        [[nodiscard]] static DungeonCatalog Load(const std::filesystem::path& inPath);

        [[nodiscard]] const std::vector<TownProtocol::DungeonOption>* FindGroup(
            std::string_view inGroupId) const;
        [[nodiscard]] bool Contains(std::string_view inGroupId, std::uint32_t inDungeonId) const;

    private:
        std::unordered_map<std::string, std::vector<TownProtocol::DungeonOption>> groups;
    };
}
