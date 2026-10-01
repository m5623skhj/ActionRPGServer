#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "DungeonDefinition.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace GameRoomServer
{
    namespace Protocol = ActionRPG::RoomControlProtocol;

    class DungeonSession;
    class GameRoom;

    class RoomManager final : public std::enable_shared_from_this<RoomManager>
    {
    public:
        using SendHandler = std::function<void(std::vector<std::uint8_t>)>;
        using CreateResultHandler = std::function<void(Protocol::CreateRoomResult)>;

        RoomManager(asio::io_context& inIoContext, Protocol::RoomServerId inRoomServerId,
            std::uint32_t inMaxRoomCount,
            std::string inSessionBrokerAddress, std::uint16_t inSessionBrokerPort,
            std::chrono::milliseconds inEnterTimeout,
            std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>> inDefinitions);

        void SetSendHandler(SendHandler inSendHandler);
        void CreateRoom(Protocol::CreateRoom inRequest, CreateResultHandler inResultHandler);
        void RegisterChallenge(std::uint64_t inChallenge, DungeonSession* inSession,
            std::uint32_t inSessionGeneration);
        void ConfirmJoin(Protocol::ConfirmJoin inRequest);
        void SessionDisconnected(std::uint64_t inChallenge, DungeonSession* inSession,
            std::uint32_t inSessionGeneration, Protocol::RoomId inRoomId, Protocol::PlayerId inPlayerId);
        void CompleteDungeon(Protocol::RoomId inRoomId);
        void Stop();

    private:
        struct PendingSession
        {
            DungeonSession* session{};
            std::uint32_t generation{};
        };

        void CompleteJoin(Protocol::RoomId inRoomId, Protocol::PlayerId inPlayerId,
            std::uint64_t inChallenge, DungeonSession* inSession, std::uint32_t inGeneration,
            bool inRoomAccepted);
        void RemoveRoom(Protocol::RoomId inRoomId, bool inAborted);
        void Send(std::vector<std::uint8_t> inPacket);
        [[nodiscard]] std::uint64_t GenerateSeed();

        asio::strand<asio::io_context::executor_type> strand;
        asio::io_context& ioContext;
        std::unordered_map<Protocol::RoomId, std::shared_ptr<GameRoom>> rooms;
        const std::unordered_map<std::uint32_t, std::shared_ptr<const DungeonDefinition>> definitions;
        std::unordered_set<Protocol::RoomId> endedRooms;
        std::unordered_map<std::uint64_t, PendingSession> pendingSessions;
        std::uint32_t maxRoomCount;
        Protocol::RoomServerId roomServerId;
        std::string sessionBrokerAddress;
        std::uint16_t sessionBrokerPort;
        std::chrono::milliseconds enterTimeout;
        SendHandler sendHandler;
        Protocol::RoomId nextRoomId = 1;
        std::uint64_t randomState;
    };
}
