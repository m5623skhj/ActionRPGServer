#include "TcpAcceptServer.h"

#include "TcpSession.h"

#include <utility>

namespace ActionRPG::Network
{
    TcpAcceptServer::TcpAcceptServer(
        asio::io_context& inIoContext,
        const asio::ip::tcp::endpoint& inEndpoint, std::shared_ptr<asio::ssl::context> inTlsContext)
        : ioContext(inIoContext),
          endpoint(inEndpoint),
          acceptor(asio::make_strand(inIoContext)), tlsContext(std::move(inTlsContext))
    {
    }

    void TcpAcceptServer::Start()
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

    void TcpAcceptServer::Stop()
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

    void TcpAcceptServer::AcceptNext()
    {
        acceptor.async_accept(asio::make_strand(ioContext),
            [this](const asio::error_code& inError, asio::ip::tcp::socket inSocket)
            {
                if (!inError)
                {
                    inSocket.set_option(asio::ip::tcp::no_delay(true));
                    const std::uint64_t sessionId = nextSessionId++;
                    std::shared_ptr<TcpSession> session = std::make_shared<TcpSession>(
                        sessionId,
                        std::move(inSocket),
                        [this](const std::uint64_t inClosedSessionId)
                        {
                            RemoveSession(inClosedSessionId);
                        }, tlsContext);
                    sessions.emplace(sessionId, session);
                    HandleAcceptedSession(std::move(session));
                }

                if (running)
                {
                    AcceptNext();
                }
            });
    }

    void TcpAcceptServer::RemoveSession(const std::uint64_t inSessionId)
    {
        asio::dispatch(acceptor.get_executor(), [this, inSessionId]()
        {
            if (sessions.erase(inSessionId) > 0)
            {
                HandleClosedSession(inSessionId);
            }
        });
    }
}
