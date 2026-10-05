#include "TownClientTcpServer.h"

#include "../Shared/TcpSession.h"
#include "PlayerSession.h"
#include "TownInstance.h"

#include <utility>

namespace TownServer::Network
{
    TownClientTcpServer::TownClientTcpServer(
        asio::io_context& inIoContext,
        const asio::ip::tcp::endpoint& inEndpoint,
        std::shared_ptr<Domain::TownInstance> inTownInstance,
        std::weak_ptr<RoomControlTcpServer> inRoomControlServer, std::shared_ptr<asio::ssl::context> inTlsContext)
        : TcpAcceptServer(inIoContext, inEndpoint, inTlsContext),
          townInstance(std::move(inTownInstance)),
          roomControlServer(std::move(inRoomControlServer))
    {
        if (!inTlsContext) throw std::invalid_argument("Town client TLS is required.");
    }

    void TownClientTcpServer::Start()
    {
        TcpAcceptServer::Start();
        townInstance->Start();
    }

    void TownClientTcpServer::Stop()
    {
        TcpAcceptServer::Stop();
        townInstance->Stop();
    }

    void TownClientTcpServer::HandleAcceptedSession(
        std::shared_ptr<ActionRPG::Network::TcpSession> inSession)
    {
        const std::uint64_t sessionId = inSession->GetSessionId();
        std::shared_ptr<PlayerSession> playerSession = std::make_shared<PlayerSession>(
            std::move(inSession), townInstance, roomControlServer);
        playerSessions.emplace(sessionId, playerSession);
        playerSession->Start();
    }

    void TownClientTcpServer::HandleClosedSession(const std::uint64_t inSessionId)
    {
        const auto iterator = playerSessions.find(inSessionId);
        if (iterator == playerSessions.end())
        {
            return;
        }
        iterator->second->Disconnect();
        playerSessions.erase(iterator);
    }
}
