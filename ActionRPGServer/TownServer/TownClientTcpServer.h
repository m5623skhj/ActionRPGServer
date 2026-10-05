#pragma once

#include "../Shared/TcpAcceptServer.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace TownServer::Domain
{
    class TownInstance;
}

namespace TownServer::Network
{
    class PlayerSession;
    class RoomControlTcpServer;

    class TownClientTcpServer final : public ActionRPG::Network::TcpAcceptServer
    {
    public:
        TownClientTcpServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint,
            std::shared_ptr<Domain::TownInstance> inTownInstance,
            std::weak_ptr<RoomControlTcpServer> inRoomControlServer,
            std::shared_ptr<asio::ssl::context> inTlsContext);

        void Start();
        void Stop();

    private:
        void HandleAcceptedSession(std::shared_ptr<ActionRPG::Network::TcpSession> inSession) override;
        void HandleClosedSession(std::uint64_t inSessionId) override;

        std::shared_ptr<Domain::TownInstance> townInstance;
        std::weak_ptr<RoomControlTcpServer> roomControlServer;
        std::unordered_map<std::uint64_t, std::shared_ptr<PlayerSession>> playerSessions;
    };
}
