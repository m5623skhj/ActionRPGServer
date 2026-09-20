#pragma once

#include "Protocol.h"

#include <filesystem>

namespace TownServer::Domain
{
    class TownMap final
    {
    public:
        [[nodiscard]] static TownMap Load(const std::filesystem::path& inPath);

        [[nodiscard]] bool IsPositionValid(TownProtocol::Vector2 inPosition) const noexcept;
        [[nodiscard]] TownProtocol::Vector2 ConstrainMovement(TownProtocol::Vector2 inPrevious,
            TownProtocol::Vector2 inProposed) const noexcept;
        [[nodiscard]] const TownProtocol::MapInfo& GetInfo() const noexcept;

    private:
        explicit TownMap(TownProtocol::MapInfo inInfo);

        TownProtocol::MapInfo info;
    };
}
