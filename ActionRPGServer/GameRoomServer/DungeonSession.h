#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "RudpPrerequisites.h"

#include <RUDPSession.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace GameRoomServer
{
    class RoomManager;

    class DungeonSession final : public RUDPSession
    {
    public:
        DungeonSession(MultiSocketRUDPCore& inCore, std::weak_ptr<RoomManager> inRoomManager);

        bool ConfirmAuthentication(ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, std::uint64_t inChallenge,
            std::uint32_t inExpectedGeneration);

    private:
        void OnConnected() override;
        void OnDisconnected() override;
        void OnReleased() override;
        [[nodiscard]] std::uint64_t GenerateChallenge() const;

        std::weak_ptr<RoomManager> roomManager;
        std::mutex bindingMutex;
        std::uint64_t challenge{};
        ActionRPG::RoomControlProtocol::RoomId roomId{};
        ActionRPG::RoomControlProtocol::PlayerId playerId{};
        std::uint32_t connectedGeneration{};
    };
}
