#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "RudpPrerequisites.h"
#include "DungeonProtocol.h"

#include <RUDPSession.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace GameRoomServer
{
    class RoomManager;
    class GameRoom;

    class DungeonSession final : public RUDPSession
    {
    public:
        DungeonSession(MultiSocketRUDPCore& inCore, std::weak_ptr<RoomManager> inRoomManager);

        bool ConfirmAuthentication(ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, std::uint64_t inChallenge,
            std::uint32_t inExpectedGeneration, std::shared_ptr<GameRoom> inGameRoom);

    private:
        void OnWorldRequest(const ActionRPG::DungeonProtocol::DungeonWorldRequest& inPacket);
        void OnMoveInput(const ActionRPG::DungeonProtocol::DungeonMoveInput& inPacket);
        void SendPlayerState(std::uint32_t inGeneration,
            ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
            ActionRPG::DungeonProtocol::DungeonPlayerState inPacket);
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
        std::weak_ptr<GameRoom> gameRoom;
        std::shared_ptr<const std::string> initialWorld;
        std::uint32_t nextWorldOffset{};
    };
}
