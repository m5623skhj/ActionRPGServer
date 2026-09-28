#pragma once

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ActionRPG::Network
{
    class TcpSession;

    class TcpAcceptServer
    {
    public:
        TcpAcceptServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint);
        virtual ~TcpAcceptServer() = default;

        void Start();
        void Stop();

    protected:
        [[nodiscard]] auto GetExecutor()
        {
            return acceptor.get_executor();
        }

        virtual void HandleAcceptedSession(std::shared_ptr<TcpSession> inSession) = 0;
        virtual void HandleClosedSession(std::uint64_t inSessionId) = 0;

    private:
        void AcceptNext();
        void RemoveSession(std::uint64_t inSessionId);

        asio::io_context& ioContext;
        asio::ip::tcp::endpoint endpoint;
        asio::ip::tcp::acceptor acceptor;
        std::unordered_map<std::uint64_t, std::shared_ptr<TcpSession>> sessions;
        std::uint64_t nextSessionId = 1;
        bool running = false;
    };
}
