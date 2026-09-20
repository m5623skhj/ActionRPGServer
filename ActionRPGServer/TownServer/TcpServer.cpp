#include "TcpServer.h"

#include "PlayerSession.h"
#include "TcpSession.h"
#include "TownInstance.h"

#include <utility>

namespace TownServer::Network
{
    TcpServer::TcpServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint,
        std::shared_ptr<Domain::TownInstance> inTownInstance)
        : ioContext(inIoContext),
          endpoint(inEndpoint),
          acceptor(asio::make_strand(inIoContext)),
          townInstance(std::move(inTownInstance))
    {
    }

    void TcpServer::Start()
    {
        if (running)
        {
            return;
        }

        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(asio::socket_base::max_listen_connections);
        running = true;
        townInstance->Start();
        AcceptNext();
    }

    void TcpServer::Stop()
    {
        asio::dispatch(acceptor.get_executor(), [this]()
        {
            if (!running)
            {
                return;
            }

            running = false;
            asio::error_code ignoredError;
            acceptor.close(ignoredError);

            for (const auto& [sessionId, session] : sessions)
            {
                session->Stop();
            }
            townInstance->Stop();
        });
    }

    void TcpServer::AcceptNext()
    {
        // A dedicated strand executor per socket serializes that session while allowing
        // different sessions to run concurrently across the I/O thread pool.
        acceptor.async_accept(asio::make_strand(ioContext), [this](const asio::error_code& inError, asio::ip::tcp::socket inSocket)
        {
            if (!inError)
            {
                inSocket.set_option(asio::ip::tcp::no_delay(true));
                const std::uint64_t sessionId = nextSessionId++;
                std::shared_ptr<TcpSession> tcpSession = std::make_shared<TcpSession>(
                    sessionId,
                    std::move(inSocket),
                    [this](const std::uint64_t inClosedSessionId)
                    {
                        RemoveSession(inClosedSessionId);
                    });

                std::shared_ptr<PlayerSession> session = std::make_shared<PlayerSession>(tcpSession, townInstance);
                sessions.emplace(sessionId, session);
                session->Start();
            }

            if (running)
            {
                AcceptNext();
            }
        });
    }

    void TcpServer::RemoveSession(const std::uint64_t inSessionId)
    {
        asio::dispatch(acceptor.get_executor(), [this, inSessionId]()
        {
            const auto iterator = sessions.find(inSessionId);
            if (iterator != sessions.end())
            {
                iterator->second->Disconnect();
                sessions.erase(iterator);
            }
        });
    }
}
