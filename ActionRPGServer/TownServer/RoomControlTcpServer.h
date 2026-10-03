#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "../Shared/TcpAcceptServer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TownServer::Domain
{
    class TownInstance;
}

namespace TownServer::Network
{
    class RoomControlTcpServer final : public ActionRPG::Network::TcpAcceptServer
    {
    public:
        using CreateRoomResultHandler = std::function<void(ActionRPG::RoomControlProtocol::CreateRoomResult)>;

        RoomControlTcpServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint,
            std::shared_ptr<Domain::TownInstance> inTownInstance);

        void CreateRoom(std::uint32_t inDungeonId,
            std::vector<ActionRPG::RoomControlProtocol::PlayerId> inParticipantPlayerIds,
            CreateRoomResultHandler inResultHandler);
        using FinishRoomResultHandler = std::function<void(ActionRPG::RoomControlProtocol::FinishRoomResult)>;
        void FinishRoom(ActionRPG::RoomControlProtocol::RoomId inRoomId, bool inRetry,
            std::vector<ActionRPG::RoomControlProtocol::PlayerId> inParticipants, FinishRoomResultHandler inHandler);
        void ConfirmJoin(ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId, std::uint64_t inChallenge, std::uint32_t inCharacterId);
        void LeaveRoom(ActionRPG::RoomControlProtocol::RoomId inRoomId,
            ActionRPG::RoomControlProtocol::PlayerId inPlayerId);

    private:
        struct RoomServerState
        {
            std::shared_ptr<ActionRPG::Network::TcpSession> session;
            ActionRPG::RoomControlProtocol::RoomServerId roomServerId{};
            std::uint32_t maxRoomCount{};
            std::uint32_t roomCount{};
            bool registered{};
        };

        struct PendingCreateRoom
        {
            std::uint64_t roomServerSessionId{};
            CreateRoomResultHandler resultHandler;
        };

        struct PendingFinishRoom
        {
            std::uint64_t roomServerSessionId{};
            ActionRPG::RoomControlProtocol::RoomId roomId{};
            FinishRoomResultHandler handler;
        };
        std::unordered_map<ActionRPG::RoomControlProtocol::RequestId, PendingFinishRoom> pendingFinishRooms;

        void HandleAcceptedSession(std::shared_ptr<ActionRPG::Network::TcpSession> inSession) override;
        void HandleClosedSession(std::uint64_t inSessionId) override;
        void HandlePacket(std::uint64_t inSessionId, std::vector<std::uint8_t> inPacket);
        void HandlePacketOnStrand(std::uint64_t inSessionId, std::vector<std::uint8_t> inPacket);
        void CloseInvalidSession(const RoomServerState& inState);

        std::shared_ptr<Domain::TownInstance> townInstance;
        const std::string authenticationKey;
        std::unordered_map<std::uint64_t, RoomServerState> roomServers;
        std::unordered_map<ActionRPG::RoomControlProtocol::RoomId, std::uint64_t> roomToServerSession;
        std::unordered_map<ActionRPG::RoomControlProtocol::RoomId,
            std::unordered_set<ActionRPG::RoomControlProtocol::PlayerId>> endingRoomPlayers;
        std::unordered_map<ActionRPG::RoomControlProtocol::RequestId, PendingCreateRoom> pendingCreateRooms;
        ActionRPG::RoomControlProtocol::RequestId nextRequestId = 1;
    };
}
