#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "RudpPrerequisites.h"
#include "DungeonProtocol.h"
#include "GameRoom.h"

#include <RUDPSession.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <chrono>

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
        void OnActionInput(const ActionRPG::DungeonProtocol::DungeonActionInput& inPacket);
        void OnSkillInput(const ActionRPG::DungeonProtocol::DungeonSkillInput& inPacket);
        void OnCombatStateRequest(const ActionRPG::DungeonProtocol::DungeonCombatStateRequest& inPacket);
        void OnRealtimeRequest(const ActionRPG::DungeonProtocol::DungeonRealtimeRequest& inPacket);
        void SendRealtimeFrame(std::uint32_t inGeneration, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, std::weak_ptr<const std::uint8_t> inLease,
            std::uint32_t inMapEpoch, std::shared_ptr<const GameRoom::RealtimeFrame> inFrame);
        void SendActionResult(std::uint32_t inGeneration, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonActionResult inPacket);
        void SendCombatChunk(std::uint32_t inGeneration, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, std::shared_ptr<const std::string> inSnapshot);
        bool SendNextCombatChunk(std::uint32_t inOffset);
        void ResetCombatStream();
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
        std::shared_ptr<const std::string> combatSnapshot;
        std::uint32_t snapshotId{}, nextCombatOffset{}, combatBytesThisSecond{}, actionsThisSecond{};
        bool snapshotPending{};
        std::chrono::steady_clock::time_point lastSnapshotRequest{}, combatWindow{}, actionWindow{};
        std::shared_ptr<const std::uint8_t> realtimeLease;
        std::uint32_t realtimeBytesThisSecond{};
        std::chrono::steady_clock::time_point realtimeWindow{}, lastRealtimeRequest{};
    };
}
