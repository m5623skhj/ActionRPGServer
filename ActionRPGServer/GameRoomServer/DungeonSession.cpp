#include "DungeonSession.h"

#include "DungeonProtocol.h"
#include "RoomManager.h"

#include <chrono>
#include <random>
#include <utility>

namespace GameRoomServer
{
    DungeonSession::DungeonSession(
        MultiSocketRUDPCore& inCore,
        std::weak_ptr<RoomManager> inRoomManager)
        : RUDPSession(inCore),
          roomManager(std::move(inRoomManager))
    {
    }

    bool DungeonSession::ConfirmAuthentication(
        const ActionRPG::RoomControlProtocol::RoomId inRoomId,
        const ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        const std::uint64_t inChallenge,
        const std::uint32_t inExpectedGeneration)
    {
        {
            std::lock_guard lock(bindingMutex);
            if (connectedGeneration != inExpectedGeneration || challenge != inChallenge
                || roomId != 0 || playerId != 0 || !IsConnected())
            {
                return false;
            }
            roomId = inRoomId;
            playerId = inPlayerId;
        }

        DungeonAuthResult result;
        result.succeeded = 1;
        if (!SendPacket(result))
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
        }
        return true;
    }

    void DungeonSession::OnConnected()
    {
        const std::uint64_t newChallenge = GenerateChallenge();
        const std::uint32_t generation = GetSessionGeneration();
        {
            std::lock_guard lock(bindingMutex);
            challenge = newChallenge;
            roomId = 0;
            playerId = 0;
            connectedGeneration = generation;
        }

        const std::shared_ptr<RoomManager> manager = roomManager.lock();
        if (!manager)
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
            return;
        }
        manager->RegisterChallenge(newChallenge, this, generation);

        DungeonChallenge packet;
        packet.challenge = newChallenge;
        if (!SendPacket(packet))
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
        }
    }

    void DungeonSession::OnDisconnected()
    {
        std::uint64_t disconnectedChallenge{};
        ActionRPG::RoomControlProtocol::RoomId disconnectedRoomId{};
        ActionRPG::RoomControlProtocol::PlayerId disconnectedPlayerId{};
        std::uint32_t disconnectedGeneration{};
        {
            std::lock_guard lock(bindingMutex);
            disconnectedChallenge = challenge;
            disconnectedRoomId = roomId;
            disconnectedPlayerId = playerId;
            disconnectedGeneration = connectedGeneration;
        }
        if (const std::shared_ptr<RoomManager> manager = roomManager.lock())
        {
            manager->SessionDisconnected(disconnectedChallenge, this, disconnectedGeneration,
                disconnectedRoomId, disconnectedPlayerId);
        }
    }

    void DungeonSession::OnReleased()
    {
        std::lock_guard lock(bindingMutex);
        challenge = 0;
        roomId = 0;
        playerId = 0;
        connectedGeneration = 0;
    }

    std::uint64_t DungeonSession::GenerateChallenge() const
    {
        std::random_device randomDevice;
        const std::uint64_t high = static_cast<std::uint64_t>(randomDevice()) << 32;
        const std::uint64_t low = randomDevice();
        const std::uint64_t time = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t result = high ^ low ^ time ^ GetSessionId();
        return result == 0 ? 1 : result;
    }
}
