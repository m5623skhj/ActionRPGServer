#include "TcpServer.h"

#include "TcpSession.h"

#include <utility>

namespace TownServer::Network
{
    TcpServer::TcpServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint)
        : ioContext(inIoContext),
          endpoint(inEndpoint),
          acceptor(asio::make_strand(inIoContext))
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
                const std::uint64_t sessionId = nextSessionId++;
                std::shared_ptr<TcpSession> session = std::make_shared<TcpSession>(
                    sessionId,
                    std::move(inSocket),
                    [this](const std::uint64_t inClosedSessionId)
                    {
                        RemoveSession(inClosedSessionId);
                    });

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
            sessions.erase(inSessionId);
        });
    }
}
